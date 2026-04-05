#include "meshtastic_tcp.h"
#include <Arduino.h>

static const char *TAG = "MeshTCP";

MeshtasticTCP::MeshtasticTCP(const char *host, uint16_t port)
    : _host(host), _port(port) {}

bool MeshtasticTCP::connect() {
    Serial.printf("[%s] Connecting to %s:%d\n", TAG, _host, _port);

    if (!_client.connect(_host, _port)) {
        Serial.printf("[%s] TCP connection failed\n", TAG);
        return false;
    }

    _connected = true;
    _config_complete = false;
    _rx_state = WAIT_START1;
    _rx_pos = 0;

    if (!_do_handshake()) {
        disconnect();
        return false;
    }

    _last_heartbeat_ms = millis();
    Serial.printf("[%s] Connected and configured, my node: %08x\n", TAG, _my_node_id);
    return true;
}

void MeshtasticTCP::disconnect() {
    if (_client.connected()) {
        // Send disconnect ToRadio { disconnect: field 4, bool true }
        uint8_t buf[4];
        int n = mesh_proto::encode_varint_field(buf, 4, 1);
        _send_raw(buf, n);
        _client.stop();
    }
    _connected = false;
    _config_complete = false;
    Serial.printf("[%s] Disconnected\n", TAG);
}

bool MeshtasticTCP::is_connected() const {
    return _connected && _config_complete && _client.connected();
}

void MeshtasticTCP::loop() {
    if (!_connected) return;

    if (!_client.connected()) {
        Serial.printf("[%s] Connection lost, will reconnect\n", TAG);
        _connected = false;
        _config_complete = false;
        return;
    }

    // Read available bytes and feed to state machine
    while (_client.available()) {
        uint8_t b = _client.read();
        _process_byte(b);
    }

    // Heartbeat
    if (_config_complete && (millis() - _last_heartbeat_ms) > MESH_HEARTBEAT_MS) {
        _send_heartbeat();
        _last_heartbeat_ms = millis();
    }
}

bool MeshtasticTCP::send_text(const char *text, uint32_t dest, uint8_t channel) {
    if (!is_connected()) return false;

    int text_len = strlen(text);
    if (text_len > MESH_DATA_MAX) text_len = MESH_DATA_MAX;

    uint8_t data_buf[MESH_MAX_PAYLOAD];
    int data_len = mesh_proto::encode_data(data_buf, PORTNUM_TEXT_MESSAGE_APP,
                                            (const uint8_t *)text, text_len);

    uint32_t pkt_id = mesh_proto::generate_packet_id(&_packet_id_counter);

    uint8_t mp_buf[MESH_MAX_PAYLOAD];
    int mp_len = mesh_proto::encode_mesh_packet(mp_buf, dest, pkt_id, channel, 3,
                                                 data_buf, data_len);

    uint8_t tr_buf[MESH_MAX_PAYLOAD];
    int tr_len = mesh_proto::encode_to_radio_packet(tr_buf, mp_buf, mp_len);

    _send_raw(tr_buf, tr_len);
    Serial.printf("[%s] Sent text (%d bytes) to %08x\n", TAG, text_len, dest);
    return true;
}

// ── Private implementation ───────────────────────────

bool MeshtasticTCP::_do_handshake() {
    // Step 1: Send 32 bytes of 0xC3 to wake/reset the device parser
    uint8_t wake[32];
    memset(wake, MESH_START2, sizeof(wake));
    _client.write(wake, sizeof(wake));
    delay(100);

    // Step 2: Request config with a random nonce
    _config_nonce = esp_random();
    uint8_t buf[16];
    int n = mesh_proto::encode_to_radio_want_config(buf, _config_nonce);
    _send_raw(buf, n);

    Serial.printf("[%s] Sent want_config_id=%08x, waiting for config...\n", TAG, _config_nonce);

    // Step 3: Wait for config_complete (with timeout)
    unsigned long start = millis();
    while (!_config_complete && (millis() - start) < 10000) {
        if (_client.available()) {
            _process_byte(_client.read());
        }
        delay(1);
    }

    if (!_config_complete) {
        Serial.printf("[%s] Config handshake timed out\n", TAG);
        return false;
    }

    return true;
}

void MeshtasticTCP::_send_raw(const uint8_t *payload, uint16_t len) {
    uint8_t header[MESH_HEADER_LEN] = {
        MESH_START1, MESH_START2,
        (uint8_t)(len >> 8), (uint8_t)(len & 0xFF)
    };
    _client.write(header, MESH_HEADER_LEN);
    _client.write(payload, len);
    _client.flush();
}

void MeshtasticTCP::_process_byte(uint8_t b) {
    switch (_rx_state) {
        case WAIT_START1:
            if (b == MESH_START1) {
                _rx_state = WAIT_START2;
            }
            // else: discard (log byte)
            break;

        case WAIT_START2:
            if (b == MESH_START2) {
                _rx_state = READ_LEN_HI;
            } else {
                _rx_state = WAIT_START1;
            }
            break;

        case READ_LEN_HI:
            _rx_payload_len = (uint16_t)b << 8;
            _rx_state = READ_LEN_LO;
            break;

        case READ_LEN_LO:
            _rx_payload_len |= b;
            if (_rx_payload_len > MESH_MAX_PAYLOAD) {
                Serial.printf("[%s] Payload too large: %d\n", TAG, _rx_payload_len);
                _rx_state = WAIT_START1;
            } else {
                _rx_pos = 0;
                _rx_state = READ_PAYLOAD;
            }
            break;

        case READ_PAYLOAD:
            _rx_buf[_rx_pos++] = b;
            if (_rx_pos >= _rx_payload_len) {
                _handle_from_radio(_rx_buf, _rx_payload_len);
                _rx_state = WAIT_START1;
            }
            break;
    }
}

void MeshtasticTCP::_handle_from_radio(const uint8_t *buf, int len) {
    // During config phase, parse for my_info BEFORE checking config_complete
    // to avoid a race where config_complete arrives before my_info is parsed.
    if (!_config_complete) {
        // Always scan for my_info (field 3) to get our node ID.
        int pos = 0;
        while (pos < len) {
            mesh_proto::ProtoField f;
            int consumed = mesh_proto::decode_field(buf + pos, len - pos, &f);
            if (consumed < 0) break;
            pos += consumed;
            if (f.field_num == 3 && f.wire_type == 2) {
                // Parse MyNodeInfo submessage for my_node_num (field 1, fixed32)
                const uint8_t *mi = f.bytes_val.data;
                int mi_len = f.bytes_val.len;
                int mi_pos = 0;
                while (mi_pos < mi_len) {
                    mesh_proto::ProtoField mf;
                    int mc = mesh_proto::decode_field(mi + mi_pos, mi_len - mi_pos, &mf);
                    if (mc < 0) break;
                    mi_pos += mc;
                    if (mf.field_num == 1 && mf.wire_type == 5) {
                        _my_node_id = mf.fixed32_val;
                        Serial.printf("[%s] Got my_node_num: %08x\n", TAG, _my_node_id);
                    }
                }
            }
        }

        // Parse NodeInfo (field 4) to learn user names
        auto ni = mesh_proto::parse_node_info(buf, len);
        if (ni.valid) {
            _store_node_info(ni);
        }

        // Now check for config_complete (after my_info has been parsed)
        if (mesh_proto::is_config_complete(buf, len, _config_nonce)) {
            _config_complete = true;
            Serial.printf("[%s] Config complete, my_node=%08x, known nodes=%d\n",
                          TAG, _my_node_id, _node_count);
        }
        return;
    }

    // Parse for text messages
    auto msg = mesh_proto::parse_from_radio(buf, len);
    if (msg.valid && _on_message) {
        // Don't echo back our own messages
        if (msg.from_node == _my_node_id && _my_node_id != 0) return;

        BridgeMessage bridge_msg;
        bridge_msg.origin = MessageOrigin::MESHTASTIC;
        bridge_msg.sender.meshtastic_node_id = msg.from_node;

        // Use resolved name if available, else fall back to hex node ID
        const char *name = get_node_name(msg.from_node);
        if (name) {
            strncpy(bridge_msg.sender.display_name, name,
                    sizeof(bridge_msg.sender.display_name) - 1);
        } else {
            snprintf(bridge_msg.sender.display_name, sizeof(bridge_msg.sender.display_name),
                     "!%08x", msg.from_node);
        }
        strncpy(bridge_msg.text, msg.text, sizeof(bridge_msg.text) - 1);
        bridge_msg.timestamp_ms = millis();

        _on_message(bridge_msg);
    }

    // Also parse NodeInfo from runtime packets (nodes joining after config)
    auto ni = mesh_proto::parse_node_info(buf, len);
    if (ni.valid) {
        _store_node_info(ni);
    }
}

void MeshtasticTCP::_store_node_info(const mesh_proto::ParsedNodeInfo &ni) {
    // Update existing entry or add new one
    for (int i = 0; i < _node_count; i++) {
        if (_nodes[i].id == ni.node_num) {
            strncpy(_nodes[i].long_name, ni.long_name, sizeof(_nodes[i].long_name) - 1);
            strncpy(_nodes[i].short_name, ni.short_name, sizeof(_nodes[i].short_name) - 1);
            Serial.printf("[%s] Updated node %08x: %s (%s)\n", TAG,
                          ni.node_num, ni.long_name, ni.short_name);
            return;
        }
    }
    if (_node_count < MAX_NODES) {
        auto &e = _nodes[_node_count++];
        e.id = ni.node_num;
        strncpy(e.long_name, ni.long_name, sizeof(e.long_name) - 1);
        strncpy(e.short_name, ni.short_name, sizeof(e.short_name) - 1);
        Serial.printf("[%s] Learned node %08x: %s (%s)\n", TAG,
                      ni.node_num, ni.long_name, ni.short_name);
    }
}

const char* MeshtasticTCP::get_node_name(uint32_t node_id) const {
    for (int i = 0; i < _node_count; i++) {
        if (_nodes[i].id == node_id && _nodes[i].long_name[0]) {
            return _nodes[i].long_name;
        }
    }
    return nullptr;
}

void MeshtasticTCP::_send_heartbeat() {
    uint8_t buf[4];
    int n = mesh_proto::encode_to_radio_heartbeat(buf);
    _send_raw(buf, n);
}
