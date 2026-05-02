#include "meshtastic_tcp.h"
#include "../utils/time_util.h"
#include <Arduino.h>
#include <ESPmDNS.h>

static const char *TAG = "MeshTCP";

MeshtasticTCP::MeshtasticTCP(const char *host, uint16_t port)
    : _host(host), _port(port) {}

bool MeshtasticTCP::connect() {
    Serial.printf("[%s] Connecting to %s:%d\n", TAG, _host, _port);

    // Resolve mDNS .local hostnames before connecting
    if (_resolve_host()) {
        Serial.printf("[%s] Resolved %s → %s\n", TAG, _host, _resolved_ip.toString().c_str());
        if (!_client.connect(_resolved_ip, _port)) {
            Serial.printf("[%s] TCP connection failed\n", TAG);
            return false;
        }
    } else if (!_client.connect(_host, _port)) {
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
        _rx_state = WAIT_START1;  // reset state machine to avoid hang
        _rx_pos = 0;
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

    // Bridge NodeInfo: emit if pending, then re-send periodically so late
    // joiners see us in their node list.
    if (_config_complete && _bridge_node_num != 0) {
        bool first  = _bridge_nodeinfo_pending;
        bool stale  = (millis() - _last_nodeinfo_ms) > BRIDGE_NODEINFO_INTERVAL_MS;
        if (first || stale) {
            _emit_node_info();
            _bridge_nodeinfo_pending = false;
            _last_nodeinfo_ms = millis();
        }
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

// ── Bridge NodeInfo upload ───────────────────────────

bool MeshtasticTCP::send_node_info(uint32_t bridge_node_num,
                                    const char *long_name,
                                    const char *short_name) {
    if (bridge_node_num == 0) return false;
    _bridge_node_num = bridge_node_num;
    if (long_name) {
        strncpy(_bridge_long_name, long_name, sizeof(_bridge_long_name) - 1);
        _bridge_long_name[sizeof(_bridge_long_name) - 1] = '\0';
    }
    if (short_name) {
        strncpy(_bridge_short_name, short_name, sizeof(_bridge_short_name) - 1);
        _bridge_short_name[sizeof(_bridge_short_name) - 1] = '\0';
    }
    _bridge_nodeinfo_pending = true;
    // If we're already connected, the next loop() tick will emit it.
    return true;
}

bool MeshtasticTCP::_emit_node_info() {
    if (!is_connected() || _bridge_node_num == 0) return false;

    // User.id format: "!XXXXXXXX" (lowercase hex of node_num).
    char id_buf[10];
    snprintf(id_buf, sizeof(id_buf), "!%08x", _bridge_node_num);

    uint8_t user_buf[128];
    int user_len = mesh_proto::encode_user(user_buf, id_buf,
                                            _bridge_long_name,
                                            _bridge_short_name,
                                            BRIDGE_HW_MODEL,
                                            BRIDGE_ROLE);

    uint8_t data_buf[MESH_MAX_PAYLOAD];
    int data_len = mesh_proto::encode_data(data_buf, PORTNUM_NODEINFO_APP,
                                            user_buf, user_len);

    uint32_t pkt_id = mesh_proto::generate_packet_id(&_packet_id_counter);

    uint8_t mp_buf[MESH_MAX_PAYLOAD];
    // Explicit `from` so the radio doesn't substitute its own node ID.
    int mp_len = mesh_proto::encode_mesh_packet(mp_buf,
                                                 _bridge_node_num,
                                                 MESH_BROADCAST,
                                                 pkt_id,
                                                 MESHTASTIC_CHANNEL,
                                                 3,
                                                 data_buf, data_len);

    uint8_t tr_buf[MESH_MAX_PAYLOAD];
    int tr_len = mesh_proto::encode_to_radio_packet(tr_buf, mp_buf, mp_len);

    _send_raw(tr_buf, tr_len);
    Serial.printf("[%s] Sent NodeInfo for !%08x (%s / %s)\n", TAG,
                  _bridge_node_num, _bridge_long_name, _bridge_short_name);
    return true;
}

// ── Private implementation ───────────────────────────

bool MeshtasticTCP::_resolve_host() {
    // Only attempt mDNS resolution for .local hostnames
    const char *suffix = ".local";
    int host_len = strlen(_host);
    int suffix_len = strlen(suffix);
    if (host_len <= suffix_len ||
        strcmp(_host + host_len - suffix_len, suffix) != 0) {
        return false;  // Not an mDNS name — let WiFiClient handle it
    }

    // Extract hostname without .local suffix
    char hostname[64];
    int name_len = host_len - suffix_len;
    if (name_len >= (int)sizeof(hostname)) name_len = sizeof(hostname) - 1;
    memcpy(hostname, _host, name_len);
    hostname[name_len] = '\0';

    Serial.printf("[%s] Resolving mDNS hostname: %s\n", TAG, _host);

    _resolved_ip = MDNS.queryHost(hostname, 5000);  // 5s timeout

    if (_resolved_ip == IPAddress(0, 0, 0, 0)) {
        Serial.printf("[%s] mDNS resolution failed for %s\n", TAG, _host);
        return false;
    }

    return true;
}

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

    // Bootstrap wall-clock time from the first Meshtastic packet with a valid timestamp.
    // This gives us real time in AP mode where NTP is unavailable.
    if (msg.rx_time > 0 && time_sync_from_epoch(msg.rx_time)) {
        Serial.printf("[%s] Clock synced from Meshtastic rx_time: %u\n", TAG, msg.rx_time);
    }

    if (msg.valid && _on_message) {
        // Don't echo back our own messages, or our own NodeInfo if we re-hear it
        if (msg.from_node == _my_node_id && _my_node_id != 0) return;
        if (msg.from_node == _bridge_node_num && _bridge_node_num != 0) return;

        // Position / Telemetry rate-limit and toggle gating.
        if (msg.kind == mesh_proto::ParsedKind::POSITION) {
            if (!BRIDGE_FORWARD_POSITION) return;
            NodeEntry *entry = _find_or_create_node(msg.from_node);
            if (entry && entry->last_pos_ms != 0 &&
                (millis() - entry->last_pos_ms) < POSITION_FORWARD_MIN_MS) {
                Serial.printf("[%s] Position rate-limited for %08x\n",
                              TAG, msg.from_node);
                return;
            }
            if (entry) entry->last_pos_ms = millis();
        } else if (msg.kind == mesh_proto::ParsedKind::TELEMETRY) {
            if (!BRIDGE_FORWARD_TELEMETRY) return;
            NodeEntry *entry = _find_or_create_node(msg.from_node);
            if (entry && entry->last_tel_ms != 0 &&
                (millis() - entry->last_tel_ms) < TELEMETRY_FORWARD_MIN_MS) {
                Serial.printf("[%s] Telemetry rate-limited for %08x\n",
                              TAG, msg.from_node);
                return;
            }
            if (entry) entry->last_tel_ms = millis();
        }

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

        // For Position/Telemetry, append rx_time HH:MM:SS so successive
        // (rate-limit-passing) reports produce distinct dedup hashes.
        if ((msg.kind == mesh_proto::ParsedKind::POSITION ||
             msg.kind == mesh_proto::ParsedKind::TELEMETRY) && msg.rx_time > 0) {
            char stamped[256];
            uint32_t s = msg.rx_time;
            unsigned hh = (s / 3600) % 24;
            unsigned mm = (s / 60) % 60;
            unsigned ss =  s        % 60;
            snprintf(stamped, sizeof(stamped), "%s @ %02u:%02u:%02u",
                     msg.text, hh, mm, ss);
            strncpy(bridge_msg.text, stamped, sizeof(bridge_msg.text) - 1);
        } else {
            strncpy(bridge_msg.text, msg.text, sizeof(bridge_msg.text) - 1);
        }
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

MeshtasticTCP::NodeEntry* MeshtasticTCP::_find_or_create_node(uint32_t node_id) {
    for (int i = 0; i < _node_count; i++) {
        if (_nodes[i].id == node_id) return &_nodes[i];
    }
    if (_node_count < MAX_NODES) {
        auto &e = _nodes[_node_count++];
        e.id = node_id;
        e.long_name[0]  = '\0';
        e.short_name[0] = '\0';
        e.last_pos_ms   = 0;
        e.last_tel_ms   = 0;
        return &e;
    }
    // Table full and node unknown — we accept missing rate-limit state for
    // overflow nodes; messages still flow, just unrate-limited.
    return nullptr;
}

void MeshtasticTCP::_send_heartbeat() {
    uint8_t buf[4];
    int n = mesh_proto::encode_to_radio_heartbeat(buf);
    _send_raw(buf, n);
}
