#pragma once

#include "bitchat_interface.h"
#include "bitchat_identity.h"
#include "noise_handshake.h"
#include "../config.h"

// Forward declarations for NimBLE types
class NimBLEServer;
class NimBLECharacteristic;
class NimBLEAdvertising;
class NimBLEConnInfo;

// ── Per-peer session ──────────────────────────────────────────────────

struct PeerSession {
    bool     active       = false;
    uint16_t conn_handle  = 0;
    uint8_t  ble_addr[6]  = {};  // peer BLE address (little-endian)
    uint8_t  peer_id[8]   = {};  // bitchat peer ID (from first received packet)
    bool     peer_id_known = false;
    NoiseHandshakeState hs = {};
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

    // TLV encode/find (also used by noise_handshake.cpp)
    static int  _tlv_encode(uint8_t *buf, uint8_t type, const uint8_t *value, uint8_t len);
    static bool _tlv_find(const uint8_t *buf, int buf_len, uint8_t type,
                           const uint8_t **value, uint8_t *len);

private:
    bool _active = false;
    BitchatKeypair _keypair = {};

    // Our own BLE address (set in begin() after NimBLE init)
    uint8_t _our_addr[6] = {};

    // BLE server — single characteristic for both directions
    NimBLEServer         *_server   = nullptr;
    NimBLECharacteristic *_msg_char = nullptr;

    // Per-peer Noise sessions
    PeerSession _peers[BITCHAT_MAX_CONNECTIONS] = {};

    // Incoming raw packet (filled by BLE write callback)
    static constexpr int RX_BUF_SIZE = BITCHAT_BLE_MTU;
    uint8_t          _rx_buf[RX_BUF_SIZE] = {};
    volatile int     _rx_len              = 0;
    volatile bool    _rx_ready            = false;
    volatile uint16_t _rx_conn_handle     = 0;

    // ── Private methods ───────────────────────────────────────────────

    void _init_ble_server();
    void _start_advertising();

    // Process whatever is in _rx_buf / _rx_len
    void _process_incoming();

    // Dispatch to individual handlers
    void _handle_plaintext(PeerSession *peer, const uint8_t *payload, int payload_len,
                           const uint8_t *sender_id, uint8_t flags);
    void _handle_noise_handshake(PeerSession *peer, const uint8_t *payload, int payload_len);
    void _handle_encrypted(PeerSession *peer, const uint8_t *pkt, int pkt_len,
                            const uint8_t *ciphertext, int ct_len);

    // Build and send a bitchat packet
    void _send_packet(uint8_t type, uint8_t flags, const uint8_t *payload, uint16_t payload_len);

    // Build and send a PKT_NOISE_HANDSHAKE packet to a specific peer (unicast)
    void _send_handshake_packet(uint16_t conn_handle,
                                const uint8_t *tlv_payload, size_t tlv_len);

    // Peer session management
    PeerSession *_find_peer(uint16_t conn_handle);
    PeerSession *_alloc_peer(uint16_t conn_handle, const uint8_t *addr);
    void         _free_peer(uint16_t conn_handle);

    // Called from server connection/disconnection callbacks (thread-safe via flag)
    void _on_connect(uint16_t conn_handle, const uint8_t *addr);
    void _on_disconnect(uint16_t conn_handle);

    // PKCS#7 padding to next block boundary
    static int  _pad_packet(uint8_t *buf, int data_len, int buf_size);

    friend class BitchatBLECallbacks;
    friend class BitchatBLEServerCallbacks;
};
