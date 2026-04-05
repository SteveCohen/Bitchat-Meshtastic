#include "bridge_manager.h"
#include <Arduino.h>

static const char *TAG = "Bridge";

BridgeManager::BridgeManager(MeshtasticInterface &mesh, BitchatInterface &bitchat,
                             const BitchatKeypair *master_kp)
    : _mesh(mesh), _bitchat(bitchat), _master_kp(master_kp) {}

bool BridgeManager::begin() {
    Serial.printf("[%s] Starting bridge...\n", TAG);

    _id_mapper.load();

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

    // Periodic re-announce of active virtual identities
    if (_master_kp && (millis() - _last_virt_announce_ms) > BITCHAT_ANNOUNCE_INTERVAL_MS) {
        _last_virt_announce_ms = millis();
        for (int i = 0; i < MAX_VIRTUAL_IDENTITIES; i++) {
            VirtualIdentity *vi = _virt_registry.slot(i);
            if (vi && vi->active) {
                _bitchat.announce_virtual(&vi->keypair,
                    vi->display_name[0] ? vi->display_name : nullptr);
                delay(VIRTUAL_ANNOUNCE_STAGGER_MS);
            }
        }
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

            _bitchat.send_text_as(msg.text, &vi->keypair);
            _msg_count++;
            return;
        }
    }

    // Fallback: send as bridge identity with prefix
    char bridged_text[256];
    _format_bridged_text(msg, bridged_text, sizeof(bridged_text));

    Serial.printf("[%s] Mesh→BLE: %s\n", TAG, bridged_text);

    BridgeMessage outgoing;
    strncpy(outgoing.text, bridged_text, sizeof(outgoing.text) - 1);
    _record_hash(outgoing.hash());

    _bitchat.send_text(bridged_text);
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

    // Format the message for Meshtastic
    char bridged_text[256];
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
