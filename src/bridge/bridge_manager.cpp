#include "bridge_manager.h"
#include <Arduino.h>

static const char *TAG = "Bridge";

BridgeManager::BridgeManager(MeshtasticInterface &mesh, BitchatInterface &bitchat)
    : _mesh(mesh), _bitchat(bitchat) {}

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

    // Format the message for Bitchat
    char bridged_text[256];
    _format_bridged_text(msg, bridged_text, sizeof(bridged_text));

    Serial.printf("[%s] Mesh→BLE: %s\n", TAG, bridged_text);

    // Record the hash of the outgoing message too (to prevent echo)
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
    const char *sender = msg.sender.display_name[0] ? msg.sender.display_name : "unknown";

    // Phase 1: simple prefix + sender + text
    // Phase 2: use IdentityMapper for richer identity mapping
    snprintf(out, out_len, "%s%s: %s", prefix, sender, msg.text);
}
