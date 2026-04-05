#pragma once

#include "bitchat_interface.h"
#include "bitchat_identity.h"
#include "noise_handshake.h"
#include "../config.h"

// Forward declarations for NimBLE types
class NimBLEServer;
class NimBLEClient;
class NimBLECharacteristic;
class NimBLERemoteCharacteristic;
class NimBLEAdvertising;
class NimBLEConnInfo;
class NimBLEAdvertisedDevice;

// ── Per-peer session ──────────────────────────────────────────────────

struct PeerSession {
    bool     active       = false;
    uint16_t conn_handle  = 0;
    uint8_t  ble_addr[6]  = {};  // peer BLE address
    uint8_t  peer_id[8]   = {};  // bitchat peer ID (from announce/first packet)
    bool     peer_id_known = false;
    bool     we_are_central = false;  // true if we connected to them
    bool     announce_sent = false;   // true after we sent our announce
    bool     announce_rcvd = false;   // true after we received their announce
    char     nickname[33]  = {};     // display name from announce (empty if unknown)
    NoiseHandshakeState hs = {};

    // For central (client) role: handle to remote characteristic
    NimBLEClient              *client     = nullptr;
    NimBLERemoteCharacteristic *remote_chr = nullptr;
};

// ── BitchatBLE class ──────────────────────────────────────────────────

class BitchatBLE : public BitchatInterface {
public:
    bool begin() override;
    void end() override;
    bool is_active() const override;
    void loop() override;
    bool send_text(const char *text) override;

    const BitchatKeypair &identity() const { return _keypair; }

    // TLV encode/find
    static int  _tlv_encode(uint8_t *buf, uint8_t type, const uint8_t *value, uint8_t len);
    static bool _tlv_find(const uint8_t *buf, int buf_len, uint8_t type,
                           const uint8_t **value, uint8_t *len);

private:
    bool _active = false;
    BitchatKeypair _keypair = {};

    // BLE server (peripheral role) — single characteristic for both directions
    NimBLEServer         *_server   = nullptr;
    NimBLECharacteristic *_msg_char = nullptr;

    // Per-peer Noise sessions (supports both central and peripheral connections)
    PeerSession _peers[BITCHAT_MAX_CONNECTIONS] = {};

    // Incoming packet ring buffer (SPSC: BLE callbacks produce, loop() consumes)
    static constexpr int RX_BUF_SIZE = BITCHAT_BLE_MTU;
    static constexpr int RX_QUEUE_SIZE = 4;
    struct RxEntry {
        uint8_t data[RX_BUF_SIZE];
        int len;
        uint16_t conn_handle;
    };
    RxEntry          _rx_queue[RX_QUEUE_SIZE] = {};
    volatile int     _rx_head = 0;  // next write position (producer)
    volatile int     _rx_tail = 0;  // next read position (consumer)

    // Legacy aliases used during _process_incoming
    uint8_t         *_rx_buf = nullptr;
    int              _rx_len = 0;
    uint16_t         _rx_conn_handle = 0;

public:
    // Scanning state (public for scan-complete free function callback)
    bool     _scanning     = false;
private:
    uint32_t _last_scan_ms = 0;
    uint32_t _last_announce_ms = 0;

    // Relay dedup cache — prevents re-forwarding packets we've already seen
    struct RelayDedupEntry { uint32_t hash; uint32_t timestamp_ms; };
    static constexpr int RELAY_DEDUP_SIZE = 64;
    RelayDedupEntry _relay_dedup[RELAY_DEDUP_SIZE] = {};
    int             _relay_dedup_idx = 0;
    uint32_t _relay_hash(const uint8_t *pkt, int pkt_len) const;
    bool     _relay_is_dup(uint32_t h) const;
    void     _relay_record(uint32_t h);

    // ── Private methods ───────────────────────────────────────────────

    void _init_ble_server(const char *name);
    void _start_advertising();
    void _start_scanning();
    void _on_scan_result(NimBLEAdvertisedDevice *dev);
    void _connect_to_peripheral(NimBLEAdvertisedDevice *dev);

    // Process whatever is in _rx_buf / _rx_len
    void _process_incoming();

    // Dispatch to individual handlers
    void _handle_announce(PeerSession *peer, const uint8_t *payload, int payload_len,
                          const uint8_t *sender_id);
    void _handle_message(PeerSession *peer, const uint8_t *payload, int payload_len,
                         const uint8_t *sender_id, uint8_t flags);
    void _handle_noise_handshake(PeerSession *peer, const uint8_t *payload, int payload_len);
    void _handle_encrypted(PeerSession *peer, const uint8_t *pkt, int hdr_len,
                            const uint8_t *ciphertext, int ct_len);

    // Build and send a bitchat packet (broadcast to all via peripheral notify)
    void _send_packet(uint8_t type, uint8_t flags, const uint8_t *payload, uint16_t payload_len);

    // Send raw bytes to a specific peer (unicast via notify or client write)
    void _send_to_peer(PeerSession *peer, const uint8_t *data, int len);

    // Build and send a PKT_NOISE_HANDSHAKE to a specific peer
    void _send_handshake_packet(PeerSession *peer, const uint8_t *payload, size_t payload_len);

    // Build and send a PKT_NOISE_ENCRYPTED to a specific peer
    void _send_encrypted(PeerSession *peer, uint8_t noise_type,
                         const uint8_t *inner, size_t inner_len);

    // Build and send an announce packet (type 0x01) to a specific peer
    void _send_announce(PeerSession *peer);
    void _send_leave();

    // Relay/gossip: forward a packet to all peers except the sender
    void _relay_packet(const uint8_t *pkt, int pkt_len, uint16_t except_handle);

    // Fragmentation: split oversized packets and reassemble incoming fragments
    void _send_fragmented(PeerSession *peer, const uint8_t *pkt, int pkt_len);
    void _handle_fragment(PeerSession *peer, const uint8_t *payload, int payload_len,
                          uint16_t conn_handle);

    // Fragment reassembly slots
    struct FragReassembly {
        bool     active = false;
        uint32_t msg_id = 0;
        uint16_t conn_handle = 0;
        uint8_t  total_frags = 0;
        uint8_t  received_mask = 0;  // bitmask of received fragments
        uint32_t start_ms = 0;
        uint8_t  data[2048] = {};
        int      frag_offsets[BITCHAT_MAX_FRAGMENTS] = {};
        int      frag_lengths[BITCHAT_MAX_FRAGMENTS] = {};
    };
    FragReassembly _frag_slots[BITCHAT_FRAG_REASSEMBLY] = {};

    // Peer session management
    PeerSession *_find_peer(uint16_t conn_handle);
    PeerSession *_alloc_peer(uint16_t conn_handle, const uint8_t *addr, bool we_are_central);
    void         _free_peer(uint16_t conn_handle);
    int          _active_peer_count() const;

    // Called from server connection/disconnection callbacks
    void _on_connect(uint16_t conn_handle, const uint8_t *addr, bool we_are_central);
    void _on_disconnect(uint16_t conn_handle);

    // PKCS#7 padding to next block boundary
    static int  _pad_packet(uint8_t *buf, int data_len, int buf_size);

    friend class BitchatBLECallbacks;
    friend class BitchatBLEServerCallbacks;
    friend class BitchatBLEScanCallbacks;
};
