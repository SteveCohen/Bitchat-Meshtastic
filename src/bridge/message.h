#pragma once

#include <cstdint>
#include <cstring>

// Unified message representation shared between both sides of the bridge.
// This decouples the Meshtastic and Bitchat transports from each other.

enum class MessageOrigin : uint8_t {
    MESHTASTIC,
    BITCHAT,
    BRIDGE_LOCAL  // originated by the bridge itself (e.g. status)
};

struct SenderIdentity {
    // Meshtastic node ID (4 bytes) — populated when origin == MESHTASTIC
    uint32_t meshtastic_node_id = 0;

    // Bitchat fingerprint (SHA-256 of Noise static public key) — populated
    // when origin == BITCHAT.  All-zeros means unknown / bridge identity.
    uint8_t bitchat_fingerprint[32] = {};

    // Human-readable short name (best-effort, may be empty)
    char display_name[32] = {};

    bool has_meshtastic_id() const { return meshtastic_node_id != 0; }
    bool has_bitchat_fingerprint() const {
        for (int i = 0; i < 32; i++) {
            if (bitchat_fingerprint[i] != 0) return true;
        }
        return false;
    }
};

struct BridgeMessage {
    MessageOrigin origin = MessageOrigin::BRIDGE_LOCAL;
    SenderIdentity sender;

    // The text payload (UTF-8, null-terminated)
    char text[256] = {};

    // Geohash scope (empty = no geohash / global scope)
    char geohash[12] = {};  // max 11 chars + null terminator

    // Geohash precision (0 = none/global)
    int geohash_precision() const {
        int len = 0;
        while (len < 11 && geohash[len]) len++;
        return len;
    }

    // Timestamp (millis since boot)
    uint32_t timestamp_ms = 0;

    // Hash for deduplication (simple djb2 of text)
    uint32_t hash() const {
        uint32_t h = 5381;
        for (const char *p = text; *p; p++) {
            h = ((h << 5) + h) + (uint8_t)*p;
        }
        // Mix in sender identity so same text from different senders isn't deduped
        h ^= sender.meshtastic_node_id;
        uint32_t bf = 0;
        memcpy(&bf, sender.bitchat_fingerprint, 4);
        h ^= bf;
        return h;
    }
};
