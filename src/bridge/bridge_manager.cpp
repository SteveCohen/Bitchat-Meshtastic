#include "bridge_manager.h"
#include <Arduino.h>

static const char *TAG = "Bridge";

BridgeManager::BridgeManager(MeshtasticInterface &mesh, BitchatInterface &bitchat,
                             const BitchatKeypair *master_kp)
    : _mesh(mesh), _bitchat(bitchat), _master_kp(master_kp) {}

bool BridgeManager::begin() {
    Serial.printf("[%s] Starting bridge...\n", TAG);

    _id_mapper.load();

    // Register eviction callback for farewell notifications
    _virt_registry.set_evict_callback([this](const VirtualIdentity &vi) {
        _on_identity_evicted(vi);
    });

    // Wire up callbacks
    _mesh.on_message([this](const BridgeMessage &msg) {
        _on_meshtastic_message(msg);
    });

    _bitchat.on_message([this](const BridgeMessage &msg) {
        _on_bitchat_message(msg);
    });

    // Initialize Bitchat BLE
    if (!_bitchat.begin()) {
        Serial.printf("[%s] Bitchat BLE init failed\n", TAG);
        return false;
    }

    // Connect to Meshtastic (may fail if WiFi isn't ready yet — that's ok,
    // loop() will retry)
    if (!_mesh.connect()) {
        Serial.printf("[%s] Meshtastic TCP connect failed (will retry)\n", TAG);
    }

    Serial.printf("[%s] Bridge started\n", TAG);
    return true;
}

void BridgeManager::loop() {
    // Drive both interfaces
    _mesh.loop();
    _bitchat.loop();

    // Non-blocking re-announce of active virtual identities (one per tick)
    if (_master_kp) {
        if (_virt_announce_idx < 0) {
            // Idle: start a new cycle when interval has elapsed
            if ((millis() - _last_virt_announce_cycle_ms) > BITCHAT_ANNOUNCE_INTERVAL_MS) {
                _virt_announce_idx = 0;
                _last_virt_announce_cycle_ms = millis();
                _last_virt_announce_step_ms = 0;  // send first immediately
            }
        }
        if (_virt_announce_idx >= 0 &&
            (millis() - _last_virt_announce_step_ms) >= VIRTUAL_ANNOUNCE_STAGGER_MS) {
            const auto &all = _virt_registry.all();
            if (_virt_announce_idx < (int)all.size()) {
                const auto &vi = all[_virt_announce_idx];
                _bitchat.announce_virtual(&vi.keypair,
                    vi.display_name[0] ? vi.display_name : nullptr);
                _last_virt_announce_step_ms = millis();
                _virt_announce_idx++;
            } else {
                _virt_announce_idx = -1;  // cycle complete
            }
        }
    }

    // Periodic memory pressure check — shed identities if heap is low
    if (millis() - _last_mem_check_ms > VIRT_ID_MEMORY_CHECK_MS) {
        _last_mem_check_ms = millis();
        _virt_registry.check_memory_pressure();
    }

    // Reconnect Meshtastic if needed
    if (!_mesh.is_connected()) {
        static unsigned long last_retry = 0;
        if (millis() - last_retry > 5000) {
            last_retry = millis();
            Serial.printf("[%s] Reconnecting to Meshtastic...\n", TAG);
            _mesh.connect();
        }
    }
}

// ── Geohash scoping ─────────────────────────────────

bool BridgeManager::_should_bridge_to_meshtastic(const BridgeMessage &msg) const {
    // If scoping disabled, always bridge (backward compatible)
    if (!GEOHASH_SCOPE_ENABLED) return true;

    // If bridge has no geohash configured, always bridge
    if (strlen(BRIDGE_GEOHASH) == 0) return true;

    // If message has no geohash, always bridge (backward compatible)
    int precision = msg.geohash_precision();
    if (precision == 0) return true;

    // Core rule: bridge only if message precision <= threshold
    // High precision = local/block scope = keep on BLE
    // Low precision = region scope = forward to Meshtastic
    if (precision <= BRIDGE_GEOHASH_PRECISION) {
        Serial.printf("[%s] Geohash '%s' (precision %d <= %d): forwarding to Meshtastic\n",
                      TAG, msg.geohash, precision, BRIDGE_GEOHASH_PRECISION);
        return true;
    }

    Serial.printf("[%s] Geohash '%s' (precision %d > %d): keeping local (BLE only)\n",
                  TAG, msg.geohash, precision, BRIDGE_GEOHASH_PRECISION);
    return false;
}

const char *BridgeManager::_bridge_region_geohash(char *buf, int buf_len) const {
    if (!GEOHASH_SCOPE_ENABLED || strlen(BRIDGE_GEOHASH) == 0) return nullptr;

    int len = BRIDGE_GEOHASH_PRECISION;
    int src_len = (int)strlen(BRIDGE_GEOHASH);
    if (len > src_len) len = src_len;
    if (len >= buf_len) len = buf_len - 1;
    memcpy(buf, BRIDGE_GEOHASH, len);
    buf[len] = '\0';
    return buf;
}

// ── Message handlers ─────────────────────────────────

void BridgeManager::_on_meshtastic_message(const BridgeMessage &msg) {
    uint32_t h = msg.hash();
    if (_is_duplicate(h)) {
        Serial.printf("[%s] Dedup: skipping Meshtastic message\n", TAG);
        return;
    }
    _record_hash(h);

    // Register sender name in identity mapper (from MeshtasticTCP's NodeInfo)
    if (msg.sender.meshtastic_node_id && msg.sender.display_name[0]) {
        _id_mapper.update_mesh_name(msg.sender.meshtastic_node_id,
                                     msg.sender.display_name, nullptr);
    }

    // Determine outgoing geohash for BLE-bound messages (region scope)
    char region_geohash[12] = {};
    const char *outgoing_geohash = _bridge_region_geohash(region_geohash, sizeof(region_geohash));

    // Try to route through a virtual identity for this Meshtastic sender
    if (_master_kp && msg.sender.meshtastic_node_id) {
        VirtualIdentity *vi = _virt_registry.get_or_create(
            msg.sender.meshtastic_node_id, _master_kp);

        if (vi) {
            // Update the virtual identity's display name
            if (msg.sender.display_name[0] && !vi->display_name[0]) {
                strncpy(vi->display_name, msg.sender.display_name,
                        sizeof(vi->display_name) - 1);
                // Announce this new virtual identity to all BLE peers
                _bitchat.announce_virtual(&vi->keypair, vi->display_name);
            }

            // Send as the virtual identity (just the raw message text, no prefix)
            Serial.printf("[%s] Mesh→BLE (virtual %s): %s\n", TAG,
                          vi->display_name[0] ? vi->display_name : "?", msg.text);

            BridgeMessage outgoing;
            strncpy(outgoing.text, msg.text, sizeof(outgoing.text) - 1);
            _record_hash(outgoing.hash());

            _bitchat.send_text_as(msg.text, &vi->keypair, outgoing_geohash);
            _msg_count++;
            return;
        }
    }

    // Fallback: send as bridge identity with prefix
    char bridged_text[320];
    _format_bridged_text(msg, bridged_text, sizeof(bridged_text));

    Serial.printf("[%s] Mesh→BLE: %s\n", TAG, bridged_text);

    BridgeMessage outgoing;
    strncpy(outgoing.text, bridged_text, sizeof(outgoing.text) - 1);
    _record_hash(outgoing.hash());

    _bitchat.send_text(bridged_text, outgoing_geohash);
    _msg_count++;
}

void BridgeManager::_on_bitchat_message(const BridgeMessage &msg) {
    uint32_t h = msg.hash();
    if (_is_duplicate(h)) {
        Serial.printf("[%s] Dedup: skipping Bitchat message\n", TAG);
        return;
    }
    _record_hash(h);

    // Register sender name in identity mapper (from bitchat announce nickname)
    if (msg.sender.display_name[0] && msg.sender.has_bitchat_fingerprint()) {
        _id_mapper.update_ble_name(msg.sender.bitchat_fingerprint,
                                    msg.sender.display_name);
    }

    // Geohash scope check: high-precision geohash = local BLE only
    if (!_should_bridge_to_meshtastic(msg)) {
        return;
    }

    // Format the message for Meshtastic
    char bridged_text[320];
    _format_bridged_text(msg, bridged_text, sizeof(bridged_text));

    Serial.printf("[%s] BLE→Mesh: %s\n", TAG, bridged_text);

    // Record the hash of the outgoing message too
    BridgeMessage outgoing;
    strncpy(outgoing.text, bridged_text, sizeof(outgoing.text) - 1);
    _record_hash(outgoing.hash());

    _mesh.send_text(bridged_text);
    _msg_count++;
}

// ── Deduplication ────────────────────────────────────

bool BridgeManager::_is_duplicate(uint32_t hash) {
    uint32_t now = millis();
    for (int i = 0; i < DEDUP_CACHE_SIZE; i++) {
        if (_dedup[i].hash == hash &&
            (now - _dedup[i].timestamp_ms) < DEDUP_TTL_MS) {
            return true;
        }
    }
    return false;
}

void BridgeManager::_record_hash(uint32_t hash) {
    _dedup[_dedup_idx].hash = hash;
    _dedup[_dedup_idx].timestamp_ms = millis();
    _dedup_idx = (_dedup_idx + 1) % DEDUP_CACHE_SIZE;
}

// ── Message formatting ───────────────────────────────

void BridgeManager::_format_bridged_text(const BridgeMessage &msg, char *out, int out_len) {
    const char *prefix = (msg.origin == MessageOrigin::MESHTASTIC) ? MSG_PREFIX_MESH : MSG_PREFIX_BLE;

    // Resolve sender name: try identity mapper first, then message display_name
    const char *sender = nullptr;
    if (msg.origin == MessageOrigin::MESHTASTIC && msg.sender.meshtastic_node_id) {
        sender = _id_mapper.get_mesh_name(msg.sender.meshtastic_node_id);
    } else if (msg.origin == MessageOrigin::BITCHAT && msg.sender.has_bitchat_fingerprint()) {
        sender = _id_mapper.get_ble_name(msg.sender.bitchat_fingerprint);
    }
    if (!sender) {
        sender = msg.sender.display_name[0] ? msg.sender.display_name : "unknown";
    }

    snprintf(out, out_len, "%s%s: %s", prefix, sender, msg.text);
}

// ── Identity eviction ───────────────────────────────

void BridgeManager::_on_identity_evicted(const VirtualIdentity &vi) {
    const char *name = vi.display_name[0] ? vi.display_name : "a Meshtastic user";

    // Notify Bitchat peers: send a farewell message as the departing identity
    char ble_msg[128];
    snprintf(ble_msg, sizeof(ble_msg),
             "%s has gone idle and left the chat. New messages from them will appear under %s.",
             name, BRIDGE_NAME);

    // Notify Meshtastic: let the user know their identity was released
    char mesh_msg[128];
    snprintf(mesh_msg, sizeof(mesh_msg),
             "[B] Bridge released identity for %s (memory pressure). "
             "Messages will still be bridged under %s.",
             name, BRIDGE_NAME);

    // Record hashes BEFORE sending to prevent echo loops
    BridgeMessage tmp;
    strncpy(tmp.text, ble_msg, sizeof(tmp.text) - 1);
    _record_hash(tmp.hash());
    memset(tmp.text, 0, sizeof(tmp.text));
    strncpy(tmp.text, mesh_msg, sizeof(tmp.text) - 1);
    _record_hash(tmp.hash());

    _bitchat.send_text_as(ble_msg, &vi.keypair);
    _mesh.send_text(mesh_msg);

    Serial.printf("[%s] Evicted virtual identity for %s (node %08x), "
                  "heap=%dKB, remaining=%d\n",
                  TAG, name, vi.mesh_node_id,
                  (int)(esp_get_free_heap_size() / 1024),
                  _virt_registry.count() - 1);
}
