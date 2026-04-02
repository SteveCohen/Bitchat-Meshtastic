#pragma once

#include "bitchat_interface.h"
#include "bitchat_identity.h"
#include "../config.h"

// Forward declarations for NimBLE types
class NimBLEServer;
class NimBLECharacteristic;
class NimBLEAdvertising;

class BitchatBLE : public BitchatInterface {
public:
    bool begin() override;
    void end() override;
    bool is_active() const override;
    void loop() override;
    bool send_text(const char *text) override;

    // Access the bridge's bitchat identity
    const BitchatKeypair &identity() const { return _keypair; }

private:
    bool _active = false;
    BitchatKeypair _keypair = {};

    // BLE server (peripheral role — other bitchat devices connect to us)
    NimBLEServer *_server = nullptr;
    // Single characteristic for both directions (write + notify)
    NimBLECharacteristic *_msg_char = nullptr;

    // Incoming message buffer (populated by BLE write callback)
    static constexpr int RX_BUF_SIZE = BITCHAT_BLE_MTU;
    uint8_t _rx_buf[RX_BUF_SIZE] = {};
    volatile int _rx_len = 0;
    volatile bool _rx_ready = false;

    // Internal methods
    void _init_ble_server();
    void _start_advertising();
    void _process_incoming();
    void _scan_for_peers();

    // TLV helpers
    static int _tlv_encode(uint8_t *buf, uint8_t type, const uint8_t *value, uint8_t len);
    static bool _tlv_find(const uint8_t *buf, int buf_len, uint8_t type,
                          const uint8_t **value, uint8_t *len);

    // PKCS#7 padding
    static int _pad_packet(uint8_t *buf, int data_len, int buf_size);

    // BLE callback friend
    friend class BitchatBLECallbacks;
};
