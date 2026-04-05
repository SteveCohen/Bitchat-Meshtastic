#pragma once

#include "../meshtastic/meshtastic_interface.h"
#include "../bitchat/bitchat_interface.h"
#include "message.h"
#include "identity_mapper.h"
#include "virtual_identity.h"
#include "../config.h"

class BridgeManager {
public:
    BridgeManager(MeshtasticInterface &mesh, BitchatInterface &bitchat,
                  const BitchatKeypair *master_kp = nullptr);

    // Initialize both sides and wire up callbacks.
    bool begin();

    // Must be called in the main loop.
    void loop();

    // Stats
    uint32_t messages_bridged() const { return _msg_count; }

private:
    MeshtasticInterface &_mesh;
    BitchatInterface &_bitchat;
    const BitchatKeypair *_master_kp = nullptr;
    IdentityMapper _id_mapper;
    VirtualIdentityRegistry _virt_registry;

    // Non-blocking virtual announce state machine
    uint32_t _last_virt_announce_cycle_ms = 0;  // when the current cycle started
    uint32_t _last_virt_announce_step_ms = 0;   // when the last individual announce was sent
    int      _virt_announce_idx = -1;           // -1 = idle, >=0 = in progress
    uint32_t _last_mem_check_ms = 0;

    // Send farewell notifications when a virtual identity is evicted
    void _on_identity_evicted(const VirtualIdentity &vi);

    uint32_t _msg_count = 0;

    // Meshtastic reconnect with exponential backoff
    uint32_t _mesh_retry_ms = 0;          // when we last attempted
    uint32_t _mesh_retry_interval = 5000; // starts at 5s, doubles up to 5min

    // Deduplication ring buffer
    struct DedupEntry {
        uint32_t hash;
        uint32_t timestamp_ms;
    };
    DedupEntry _dedup[DEDUP_CACHE_SIZE] = {};
    int _dedup_idx = 0;

    // Handlers
    void _on_meshtastic_message(const BridgeMessage &msg);
    void _on_bitchat_message(const BridgeMessage &msg);

    // Deduplication
    bool _is_duplicate(uint32_t hash);
    void _record_hash(uint32_t hash);

    // Format bridged message text with origin prefix and sender info
    void _format_bridged_text(const BridgeMessage &msg, char *out, int out_len);
};
