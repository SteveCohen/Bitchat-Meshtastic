#pragma once

#include "../bitchat/bitchat_identity.h"
#include "../config.h"
#include <Arduino.h>

// ── Virtual Identity Registry ───────────────────────────────────────
//
// Manages derived bitchat identities for Meshtastic users.
// Each active Meshtastic sender gets a deterministic virtual keypair
// so they appear as a distinct bitchat peer.

struct VirtualIdentity {
    bool active = false;
    uint32_t mesh_node_id = 0;
    BitchatKeypair keypair = {};
    uint32_t last_seen_ms = 0;
    char display_name[40] = {};  // Meshtastic long_name if known
};

class VirtualIdentityRegistry {
public:
    // Get or create a virtual identity for a Meshtastic node.
    // Returns nullptr if all slots are full (after expiry attempt).
    VirtualIdentity* get_or_create(uint32_t node_id, const BitchatKeypair *master) {
        // Return existing
        for (int i = 0; i < MAX_VIRTUAL_IDENTITIES; i++) {
            if (_slots[i].active && _slots[i].mesh_node_id == node_id) {
                _slots[i].last_seen_ms = millis();
                return &_slots[i];
            }
        }

        // Expire stale entries to make room
        expire_stale(VIRTUAL_IDENTITY_TIMEOUT_MS);

        // Find free slot
        for (int i = 0; i < MAX_VIRTUAL_IDENTITIES; i++) {
            if (!_slots[i].active) {
                auto &v = _slots[i];
                v.active = true;
                v.mesh_node_id = node_id;
                v.last_seen_ms = millis();
                v.display_name[0] = '\0';

                if (!bitchat_derive_keypair(&v.keypair, master, node_id)) {
                    v.active = false;
                    return nullptr;
                }

                char fp[9];
                bitchat_fingerprint_short(v.keypair.fingerprint, fp, sizeof(fp));
                Serial.printf("[VirtId] Created virtual identity for mesh node %08x (fp=%s)\n",
                              node_id, fp);
                return &v;
            }
        }

        return nullptr;  // all slots occupied
    }

    // Look up an existing virtual identity (no creation).
    VirtualIdentity* find(uint32_t node_id) {
        for (int i = 0; i < MAX_VIRTUAL_IDENTITIES; i++) {
            if (_slots[i].active && _slots[i].mesh_node_id == node_id) {
                return &_slots[i];
            }
        }
        return nullptr;
    }

    // Remove stale entries older than timeout_ms.
    void expire_stale(uint32_t timeout_ms) {
        uint32_t now = millis();
        for (int i = 0; i < MAX_VIRTUAL_IDENTITIES; i++) {
            if (_slots[i].active &&
                (now - _slots[i].last_seen_ms) > timeout_ms) {
                Serial.printf("[VirtId] Expired virtual identity for mesh node %08x\n",
                              _slots[i].mesh_node_id);
                _slots[i] = VirtualIdentity{};
            }
        }
    }

    // Iterate over all active identities (for announcing).
    int count() const {
        int n = 0;
        for (int i = 0; i < MAX_VIRTUAL_IDENTITIES; i++) {
            if (_slots[i].active) n++;
        }
        return n;
    }

    VirtualIdentity* slot(int i) {
        return (i >= 0 && i < MAX_VIRTUAL_IDENTITIES) ? &_slots[i] : nullptr;
    }

private:
    VirtualIdentity _slots[MAX_VIRTUAL_IDENTITIES] = {};
};
