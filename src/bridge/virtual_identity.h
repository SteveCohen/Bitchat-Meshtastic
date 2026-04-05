#pragma once

#include "../bitchat/bitchat_identity.h"
#include "../config.h"
#include <Arduino.h>
#include <vector>
#include <functional>

// ── Virtual Identity Registry ───────────────────────────────────────
//
// Manages derived bitchat identities for Meshtastic users. Scales
// dynamically based on available heap memory rather than using a fixed
// slot count. When memory pressure is high, the least-recently-used
// identities are evicted and farewell notifications are sent.

struct VirtualIdentity {
    uint32_t mesh_node_id = 0;
    BitchatKeypair keypair = {};
    uint32_t last_seen_ms = 0;
    char display_name[40] = {};
};

class VirtualIdentityRegistry {
public:
    // Callback invoked when an identity is about to be evicted.
    // The caller should send farewell notifications before returning.
    using EvictCallback = std::function<void(const VirtualIdentity &vi)>;

    void set_evict_callback(EvictCallback cb) { _on_evict = cb; }

    // Get or create a virtual identity for a Meshtastic node.
    // Returns nullptr only if heap is critically low and no stale entries
    // can be evicted.
    VirtualIdentity* get_or_create(uint32_t node_id, const BitchatKeypair *master) {
        // Return existing
        for (auto &vi : _identities) {
            if (vi.mesh_node_id == node_id) {
                vi.last_seen_ms = millis();
                return &vi;
            }
        }

        // Try to make room if we can't grow
        if (!_can_grow()) {
            // Expire stale entries first
            _expire_stale(VIRT_ID_TIMEOUT_MS);

            // Still can't grow? Evict least-recently-used
            if (!_can_grow() && !_identities.empty()) {
                _evict_lru();
            }

            // If still can't grow, give up
            if (!_can_grow()) {
                return nullptr;
            }
        }

        // Create new identity
        VirtualIdentity vi;
        vi.mesh_node_id = node_id;
        vi.last_seen_ms = millis();
        vi.display_name[0] = '\0';

        if (!bitchat_derive_keypair(&vi.keypair, master, node_id)) {
            return nullptr;
        }

        _identities.push_back(vi);

        char fp[9];
        bitchat_fingerprint_short(vi.keypair.fingerprint, fp, sizeof(fp));
        Serial.printf("[VirtId] Created identity for node %08x (fp=%s) [%d/%d, heap=%dKB]\n",
                      node_id, fp, (int)_identities.size(), VIRT_ID_MAX_SLOTS,
                      (int)(esp_get_free_heap_size() / 1024));

        return &_identities.back();
    }

    // Look up an existing virtual identity (no creation).
    VirtualIdentity* find(uint32_t node_id) {
        for (auto &vi : _identities) {
            if (vi.mesh_node_id == node_id) return &vi;
        }
        return nullptr;
    }

    // Check heap pressure and shed identities if needed.
    // Call periodically from the main loop.
    // Returns the number of identities evicted.
    int check_memory_pressure() {
        int evicted = 0;
        uint32_t free_heap = esp_get_free_heap_size();

        if (free_heap >= VIRT_ID_HEAP_CRITICAL_BYTES || _identities.empty()) {
            return 0;
        }

        Serial.printf("[VirtId] Memory pressure: %dKB free (critical=%dKB), shedding identities\n",
                      (int)(free_heap / 1024), VIRT_ID_HEAP_CRITICAL_BYTES / 1024);

        // Evict LRU entries until we're above the reserve threshold or empty
        while (!_identities.empty() && esp_get_free_heap_size() < VIRT_ID_HEAP_RESERVE_BYTES) {
            _evict_lru();
            evicted++;
        }

        return evicted;
    }

    int count() const { return (int)_identities.size(); }
    int capacity() const { return _current_max(); }

    // Iterate over all active identities (for announcing).
    VirtualIdentity* at(int i) {
        return (i >= 0 && i < (int)_identities.size()) ? &_identities[i] : nullptr;
    }

    const std::vector<VirtualIdentity>& all() const { return _identities; }

private:
    std::vector<VirtualIdentity> _identities;
    EvictCallback _on_evict;

    // Determine the current max based on heap headroom.
    int _current_max() const {
        uint32_t free_heap = esp_get_free_heap_size();
        if (free_heap < VIRT_ID_HEAP_RESERVE_BYTES) return (int)_identities.size();

        int headroom_slots = (free_heap - VIRT_ID_HEAP_RESERVE_BYTES) / sizeof(VirtualIdentity);
        int total = (int)_identities.size() + headroom_slots;
        return (total < VIRT_ID_MAX_SLOTS) ? total : VIRT_ID_MAX_SLOTS;
    }

    bool _can_grow() const {
        if ((int)_identities.size() >= VIRT_ID_MAX_SLOTS) return false;
        uint32_t free_heap = esp_get_free_heap_size();
        return free_heap > VIRT_ID_HEAP_RESERVE_BYTES;
    }

    // Remove entries idle longer than timeout_ms.
    void _expire_stale(uint32_t timeout_ms) {
        uint32_t now = millis();
        auto it = _identities.begin();
        while (it != _identities.end()) {
            if ((now - it->last_seen_ms) > timeout_ms) {
                Serial.printf("[VirtId] Expired idle identity: %s (node %08x)\n",
                              it->display_name[0] ? it->display_name : "?", it->mesh_node_id);
                if (_on_evict) _on_evict(*it);
                it = _identities.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Evict the least-recently-used identity.
    void _evict_lru() {
        if (_identities.empty()) return;

        auto lru = _identities.begin();
        for (auto it = _identities.begin(); it != _identities.end(); ++it) {
            if (it->last_seen_ms < lru->last_seen_ms) {
                lru = it;
            }
        }

        Serial.printf("[VirtId] Evicting LRU identity: %s (node %08x, idle %ds)\n",
                      lru->display_name[0] ? lru->display_name : "?",
                      lru->mesh_node_id,
                      (int)((millis() - lru->last_seen_ms) / 1000));

        if (_on_evict) _on_evict(*lru);
        _identities.erase(lru);
    }
};
