#include "meshtastic_udp.h"
#include "mesh_crypto.h"
#include "../utils/time_util.h"
#include <Arduino.h>
#include <esp_mac.h>
#include <cstring>

static const char *TAG = "MeshUDP";

// ── Lifecycle ────────────────────────────────────────

bool MeshtasticUDP::connect() {
    if (_running) return true;

    if (WiFi.status() != WL_CONNECTED && !WiFi.softAPgetStationNum()) {
        // Either STA needs to be up, or we're hosting an AP; UDP multicast
        // requires the network stack initialized either way.
        if (WiFi.status() != WL_CONNECTED && WiFi.getMode() != WIFI_AP &&
            WiFi.getMode() != WIFI_AP_STA) {
            Serial.printf("[%s] WiFi not ready; deferring multicast join\n", TAG);
            return false;
        }
    }

    if (_my_node_num == 0) _derive_node_num_from_mac();
    if (_psk_len == 0) {
        if (!_load_psk_from_hex(MESH_CHANNEL_PSK_HEX)) {
            // Fall back to the LongFast default (16 bytes).
            memcpy(_psk, MESH_DEFAULT_PSK, sizeof(MESH_DEFAULT_PSK));
            _psk_len = sizeof(MESH_DEFAULT_PSK);
            Serial.printf("[%s] PSK hex parse failed; using LongFast default\n", TAG);
        }
    }

    IPAddress group;
    if (!group.fromString(MESH_UDP_GROUP)) {
        Serial.printf("[%s] Bad multicast group: %s\n", TAG, MESH_UDP_GROUP);
        return false;
    }

    if (!_udp.beginMulticast(group, MESH_UDP_PORT)) {
        Serial.printf("[%s] beginMulticast(%s:%u) failed\n",
                      TAG, MESH_UDP_GROUP, MESH_UDP_PORT);
        return false;
    }

    _running = true;
    Serial.printf("[%s] Joined %s:%u, my_node_num=!%08x, psk=%u bytes\n",
                  TAG, MESH_UDP_GROUP, MESH_UDP_PORT, _my_node_num,
                  (unsigned)_psk_len);
    return true;
}

void MeshtasticUDP::disconnect() {
    if (!_running) return;
    _udp.stop();
    _running = false;
}

bool MeshtasticUDP::is_connected() const {
    return _running && (WiFi.status() == WL_CONNECTED ||
                        WiFi.getMode() == WIFI_AP ||
                        WiFi.getMode() == WIFI_AP_STA);
}

// ── Receive loop ─────────────────────────────────────

void MeshtasticUDP::loop() {
    if (!_running) {
        // Try to start once WiFi comes up.
        connect();
        return;
    }

    // Drain whatever's available this tick.
    int sz;
    while ((sz = _udp.parsePacket()) > 0) {
        if (sz > MESH_MAX_PAYLOAD) {
            // Unexpectedly large; flush and skip.
            _udp.flush();
            continue;
        }
        uint8_t buf[MESH_MAX_PAYLOAD];
        int n = _udp.read(buf, sz);
        if (n <= 0) continue;
        _process_datagram(buf, n, _udp.remoteIP());
    }

    // Periodic NodeInfo re-broadcast (so phone apps populate the bridge).
    if (_my_node_num != 0 && _bridge_long_name[0]) {
        uint32_t now = millis();
        if (_last_nodeinfo_ms == 0 ||
            (now - _last_nodeinfo_ms) > BRIDGE_NODEINFO_INTERVAL_MS) {
            send_node_info(_my_node_num, _bridge_long_name, _bridge_short_name);
        }
    }
}

void MeshtasticUDP::_process_datagram(const uint8_t *buf, int len,
                                      IPAddress /*src*/) {
    auto mp = mesh_proto::parse_mesh_packet(buf, len);
    if (!mp.valid) return;

    // Drop our own loopback first.
    if (mp.from_node == _my_node_num || _is_self_id(mp.packet_id)) return;

    // Decrypt the SubPacket.
    uint8_t pt[MESH_MAX_PAYLOAD];
    if (mp.encrypted_len > (int)sizeof(pt)) return;
    int rc = mesh_crypt(mp.packet_id, mp.from_node,
                        _psk, _psk_len,
                        mp.encrypted, pt, mp.encrypted_len);
    if (rc != 0) {
        Serial.printf("[%s] decrypt failed (rc=%d) from=!%08x id=%08x\n",
                      TAG, rc, mp.from_node, mp.packet_id);
        return;
    }

    mesh_proto::ParsedTextMessage msg;
    if (!mesh_proto::parse_data_subpacket(pt, mp.encrypted_len, msg)) {
        // Most non-text packets land here — silent drop.
        return;
    }

    BridgeMessage out;
    out.origin       = MessageOrigin::MESHTASTIC;
    out.timestamp_ms = millis();
    out.sender.meshtastic_node_id = mp.from_node;
    snprintf(out.sender.display_name, sizeof(out.sender.display_name),
             "!%08x", mp.from_node);
    int tlen = (int)strnlen(msg.text, sizeof(msg.text));
    int copy = (tlen < (int)sizeof(out.text) - 1)
                  ? tlen : (int)sizeof(out.text) - 1;
    memcpy(out.text, msg.text, copy);
    out.text[copy] = '\0';

    Serial.printf("[%s] Message from !%08x: \"%s\"\n", TAG,
                  mp.from_node, out.text);
    if (_on_message) _on_message(out);
}

// ── Send paths ───────────────────────────────────────

bool MeshtasticUDP::send_text(const char *text, uint32_t dest, uint8_t channel) {
    if (!is_connected() || !text) return false;
    uint8_t data_buf[MESH_MAX_PAYLOAD];
    int data_len = mesh_proto::encode_data(data_buf, PORTNUM_TEXT_MESSAGE_APP,
                                            (const uint8_t *)text,
                                            (int)strlen(text));
    return _send_packet(dest, channel, 3, data_buf, data_len);
}

bool MeshtasticUDP::send_node_info(uint32_t bridge_node_num,
                                    const char *long_name,
                                    const char *short_name) {
    if (bridge_node_num == 0) return false;
    if (long_name) {
        strncpy(_bridge_long_name, long_name, sizeof(_bridge_long_name) - 1);
        _bridge_long_name[sizeof(_bridge_long_name) - 1] = '\0';
    }
    if (short_name) {
        strncpy(_bridge_short_name, short_name, sizeof(_bridge_short_name) - 1);
        _bridge_short_name[sizeof(_bridge_short_name) - 1] = '\0';
    }
    _my_node_num = bridge_node_num;

    if (!is_connected()) return true;  // will retry next loop()

    char id_buf[10];
    snprintf(id_buf, sizeof(id_buf), "!%08x", bridge_node_num);
    uint8_t user_buf[128];
    int user_len = mesh_proto::encode_user(user_buf, id_buf,
                                            _bridge_long_name,
                                            _bridge_short_name,
                                            BRIDGE_HW_MODEL, BRIDGE_ROLE);
    uint8_t data_buf[MESH_MAX_PAYLOAD];
    int data_len = mesh_proto::encode_data(data_buf, PORTNUM_NODEINFO_APP,
                                            user_buf, user_len);
    bool ok = _send_packet(MESH_BROADCAST, MESHTASTIC_CHANNEL, 3,
                           data_buf, data_len);
    if (ok) {
        _last_nodeinfo_ms = millis();
        Serial.printf("[%s] Sent NodeInfo for !%08x (%s / %s)\n", TAG,
                      bridge_node_num, _bridge_long_name, _bridge_short_name);
    }
    return ok;
}

bool MeshtasticUDP::_send_packet(uint32_t to, uint8_t channel,
                                  uint8_t hop_limit,
                                  const uint8_t *data_buf, int data_len) {
    uint32_t pkt_id = mesh_proto::generate_packet_id(&_packet_id_counter);

    // Encrypt the Data subpacket bytes in place.
    uint8_t enc[MESH_MAX_PAYLOAD];
    if (data_len > (int)sizeof(enc)) return false;
    int rc = mesh_crypt(pkt_id, _my_node_num, _psk, _psk_len,
                        data_buf, enc, data_len);
    if (rc != 0) {
        Serial.printf("[%s] encrypt failed rc=%d\n", TAG, rc);
        return false;
    }

    uint8_t mp_buf[MESH_MAX_PAYLOAD];
    int mp_len = mesh_proto::encode_mesh_packet_encrypted(
        mp_buf, _my_node_num, to, pkt_id, channel, hop_limit, enc, data_len);

    IPAddress group;
    group.fromString(MESH_UDP_GROUP);
    if (!_udp.beginPacket(group, MESH_UDP_PORT)) return false;
    _udp.write(mp_buf, mp_len);
    bool ok = _udp.endPacket();
    if (ok) _record_self_id(pkt_id);
    return ok;
}

// ── Loopback dedup ───────────────────────────────────

bool MeshtasticUDP::_is_self_id(uint32_t id) const {
    for (int i = 0; i < LOOPBACK_RING; i++) {
        if (_self_ids[i] == id) return true;
    }
    return false;
}

void MeshtasticUDP::_record_self_id(uint32_t id) {
    _self_ids[_self_ids_idx] = id;
    _self_ids_idx = (_self_ids_idx + 1) % LOOPBACK_RING;
}

// ── Identity / config helpers ────────────────────────

void MeshtasticUDP::_derive_node_num_from_mac() {
#if defined(BRIDGE_MESH_NODE_NUM) && (BRIDGE_MESH_NODE_NUM != 0)
    _my_node_num = (uint32_t)BRIDGE_MESH_NODE_NUM;
    return;
#else
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BT);
    // Pack the low 4 bytes of the BLE MAC, mask MSB so it stays in the
    // positive int32 range that some Meshtastic clients still expect.
    uint32_t n = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                 ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
    n &= 0x7FFFFFFFu;
    if (n == 0)              n = 1;
    if (n == MESH_BROADCAST) n ^= 0x12345678u;
    _my_node_num = n;
#endif
}

bool MeshtasticUDP::_load_psk_from_hex(const char *hex) {
    if (!hex) return false;
    size_t hlen = strlen(hex);
    if (hlen != 32 && hlen != 64) return false;  // 16 or 32 bytes
    size_t bytes = hlen / 2;
    for (size_t i = 0; i < bytes; i++) {
        char hi = hex[i*2], lo = hex[i*2 + 1];
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nibble(hi), l = nibble(lo);
        if (h < 0 || l < 0) return false;
        _psk[i] = (uint8_t)((h << 4) | l);
    }
    _psk_len = bytes;
    return true;
}
