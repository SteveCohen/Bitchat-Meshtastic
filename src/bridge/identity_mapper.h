#pragma once

#include <cstdint>

// ── Phase 2 Placeholder ──────────────────────────────────────────────
//
// IdentityMapper will maintain a bidirectional mapping between Meshtastic
// node IDs and Bitchat fingerprints, allowing per-user identity on both
// sides of the bridge.
//
// Phase 1: all messages use the single bridge identity.
// Phase 2: each user gets a virtual identity on the other network.

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
    // Map a Meshtastic node ID to a Bitchat display identity.
    // Phase 1: returns the bridge's own identity for all nodes.
    BitchatIdentity mesh_to_bitchat(uint32_t meshtastic_node_id) {
        (void)meshtastic_node_id;
        // TODO Phase 2: look up or create a virtual bitchat identity
        BitchatIdentity id = {};
        snprintf(id.display_name, sizeof(id.display_name), "mesh_%08x", meshtastic_node_id);
        return id;
    }

    // Map a Bitchat fingerprint to a Meshtastic display identity.
    // Phase 1: returns a generic bridge identity for all bitchat users.
    MeshtasticIdentity bitchat_to_mesh(const uint8_t fingerprint[32]) {
        (void)fingerprint;
        // TODO Phase 2: look up or create a virtual Meshtastic identity
        MeshtasticIdentity id = {};
        id.node_id = 0;
        snprintf(id.short_name, sizeof(id.short_name), "BC");
        snprintf(id.long_name, sizeof(id.long_name), "Bitchat User");
        return id;
    }

    // Persist mappings to NVS (non-volatile storage).
    void save() {
        // TODO Phase 2: implement NVS persistence
    }

    void load() {
        // TODO Phase 2: implement NVS loading
    }
};
