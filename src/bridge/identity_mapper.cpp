#include "identity_mapper.h"
#include <Arduino.h>
#include <nvs_flash.h>
#include <nvs.h>

static const char *TAG = "IdMap";

// ── Name resolution ─────────────────────────────────────

const char* IdentityMapper::get_mesh_name(uint32_t node_id) const {
    for (int i = 0; i < _mesh_count; i++) {
        if (_mesh_names[i].node_id == node_id && _mesh_names[i].long_name[0]) {
            return _mesh_names[i].long_name;
        }
    }
    return nullptr;
}

const char* IdentityMapper::get_ble_name(const uint8_t fingerprint[8]) const {
    for (int i = 0; i < _ble_count; i++) {
        if (memcmp(_ble_names[i].fingerprint, fingerprint, 8) == 0 &&
            _ble_names[i].nickname[0]) {
            return _ble_names[i].nickname;
        }
    }
    return nullptr;
}

// ── Name registration ───────────────────────────────────

void IdentityMapper::update_mesh_name(uint32_t node_id, const char *long_name, const char *short_name) {
    // Update existing entry
    for (int i = 0; i < _mesh_count; i++) {
        if (_mesh_names[i].node_id == node_id) {
            if (long_name) strncpy(_mesh_names[i].long_name, long_name, sizeof(_mesh_names[i].long_name) - 1);
            if (short_name) strncpy(_mesh_names[i].short_name, short_name, sizeof(_mesh_names[i].short_name) - 1);
            _dirty = true;
            _save_if_needed();
            return;
        }
    }
    // Add new entry
    if (_mesh_count < MAX_IDENTITY_ENTRIES) {
        auto &e = _mesh_names[_mesh_count++];
        e.node_id = node_id;
        if (long_name) strncpy(e.long_name, long_name, sizeof(e.long_name) - 1);
        if (short_name) strncpy(e.short_name, short_name, sizeof(e.short_name) - 1);
        _dirty = true;
        _save_if_needed();
    }
}

void IdentityMapper::update_ble_name(const uint8_t fingerprint[8], const char *nickname) {
    // Update existing entry
    for (int i = 0; i < _ble_count; i++) {
        if (memcmp(_ble_names[i].fingerprint, fingerprint, 8) == 0) {
            if (nickname) strncpy(_ble_names[i].nickname, nickname, sizeof(_ble_names[i].nickname) - 1);
            _dirty = true;
            _save_if_needed();
            return;
        }
    }
    // Add new entry
    if (_ble_count < MAX_IDENTITY_ENTRIES) {
        auto &e = _ble_names[_ble_count++];
        memcpy(e.fingerprint, fingerprint, 8);
        if (nickname) strncpy(e.nickname, nickname, sizeof(e.nickname) - 1);
        _dirty = true;
        _save_if_needed();
    }
}

// ── Legacy Phase 1 compatibility ────────────────────────

BitchatIdentity IdentityMapper::mesh_to_bitchat(uint32_t meshtastic_node_id) {
    BitchatIdentity id = {};
    const char *name = get_mesh_name(meshtastic_node_id);
    if (name) {
        snprintf(id.display_name, sizeof(id.display_name), "%s", name);
    } else {
        snprintf(id.display_name, sizeof(id.display_name), "mesh_%08x", meshtastic_node_id);
    }
    return id;
}

MeshtasticIdentity IdentityMapper::bitchat_to_mesh(const uint8_t fingerprint[32]) {
    MeshtasticIdentity id = {};
    id.node_id = 0;
    const char *name = get_ble_name(fingerprint);
    if (name) {
        snprintf(id.long_name, sizeof(id.long_name), "%s", name);
        // Derive short_name from first 4 chars of nickname
        strncpy(id.short_name, name, sizeof(id.short_name) - 1);
    } else {
        snprintf(id.short_name, sizeof(id.short_name), "BC");
        snprintf(id.long_name, sizeof(id.long_name), "Bitchat User");
    }
    return id;
}

// ── Persistence ─────────────────────────────────────────

void IdentityMapper::_save_if_needed() {
    if (!_dirty) return;
    uint32_t now = millis();
    if (_last_save_ms != 0 && (now - _last_save_ms) < IDENTITY_SAVE_DEBOUNCE_MS) return;
    save();
}

void IdentityMapper::save() {
    if (!_dirty) return;

    nvs_handle_t h;
    if (nvs_open(IDENTITY_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        Serial.printf("[%s] NVS open failed for save\n", TAG);
        return;
    }

    // Save mesh names as a single blob
    nvs_set_blob(h, "mesh", _mesh_names, sizeof(MeshEntry) * _mesh_count);
    nvs_set_u8(h, "mesh_n", (uint8_t)_mesh_count);

    // Save BLE names as a single blob
    nvs_set_blob(h, "ble", _ble_names, sizeof(BleEntry) * _ble_count);
    nvs_set_u8(h, "ble_n", (uint8_t)_ble_count);

    nvs_commit(h);
    nvs_close(h);

    _dirty = false;
    _last_save_ms = millis();
    Serial.printf("[%s] Saved %d mesh + %d BLE identities to NVS\n",
                  TAG, _mesh_count, _ble_count);
}

void IdentityMapper::load() {
    nvs_handle_t h;
    if (nvs_open(IDENTITY_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        // No saved data yet — that's fine
        return;
    }

    uint8_t count = 0;
    size_t blob_size = 0;

    // Load mesh names
    if (nvs_get_u8(h, "mesh_n", &count) == ESP_OK && count <= MAX_IDENTITY_ENTRIES) {
        blob_size = sizeof(MeshEntry) * count;
        if (nvs_get_blob(h, "mesh", _mesh_names, &blob_size) == ESP_OK) {
            _mesh_count = count;
        }
    }

    // Load BLE names
    if (nvs_get_u8(h, "ble_n", &count) == ESP_OK && count <= MAX_IDENTITY_ENTRIES) {
        blob_size = sizeof(BleEntry) * count;
        if (nvs_get_blob(h, "ble", _ble_names, &blob_size) == ESP_OK) {
            _ble_count = count;
        }
    }

    nvs_close(h);
    Serial.printf("[%s] Loaded %d mesh + %d BLE identities from NVS\n",
                  TAG, _mesh_count, _ble_count);
}
