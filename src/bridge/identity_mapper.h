#pragma once

#include <cstdint>
#include <cstring>
#include "../config.h"

// ── Identity Mapper ─────────────────────────────────────────────────────
//
// Maintains a bidirectional name cache: Meshtastic node IDs → display names,
// and Bitchat fingerprints → nicknames. Persists to NVS across reboots.

struct BitchatIdentity {
    uint8_t fingerprint[32];  // SHA-256 of Noise static public key
    uint8_t noise_pubkey[32]; // Curve25519 public key
    uint8_t sign_pubkey[32];  // Ed25519 public key
    char display_name[32];
};

struct MeshtasticIdentity {
    uint32_t node_id;
    char short_name[5];  // Meshtastic 4-char short name
    char long_name[40];
};

class IdentityMapper {
public:
    // ── Name resolution ─────────────────────────────────

    // Look up a Meshtastic node's display name. Returns nullptr if unknown.
    const char* get_mesh_name(uint32_t node_id) const;

    // Look up a Bitchat peer's nickname. Returns nullptr if unknown.
    const char* get_ble_name(const uint8_t fingerprint[8]) const;

    // ── Name registration ───────────────────────────────

    // Store/update a Meshtastic node name (from NodeInfo parsing).
    void update_mesh_name(uint32_t node_id, const char *long_name, const char *short_name);

    // Store/update a Bitchat peer nickname (from announce packets).
    void update_ble_name(const uint8_t fingerprint[8], const char *nickname);

    // ── Legacy Phase 1 compatibility ────────────────────

    BitchatIdentity mesh_to_bitchat(uint32_t meshtastic_node_id);
    MeshtasticIdentity bitchat_to_mesh(const uint8_t fingerprint[32]);

    // ── Persistence ─────────────────────────────────────

    void save();
    void load();

private:
    struct MeshEntry {
        uint32_t node_id = 0;
        char long_name[40] = {};
        char short_name[5] = {};
    };

    struct BleEntry {
        uint8_t fingerprint[8] = {};
        char nickname[33] = {};
    };

    MeshEntry _mesh_names[MAX_IDENTITY_ENTRIES] = {};
    int _mesh_count = 0;

    BleEntry _ble_names[MAX_IDENTITY_ENTRIES] = {};
    int _ble_count = 0;

    uint32_t _last_save_ms = 0;
    bool _dirty = false;

    void _save_if_needed();
};
