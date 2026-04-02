#include "bitchat_ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

static const char *TAG = "BitchatBLE";

// ── BLE Write Callback ───────────────────────────────
// Called when a remote bitchat device writes a message to our RX characteristic.

class BitchatBLECallbacks : public NimBLECharacteristicCallbacks {
public:
    BitchatBLECallbacks(BitchatBLE *parent) : _parent(parent) {}

    void onWrite(NimBLECharacteristic *characteristic) override {
        NimBLEAttValue val = characteristic->getValue();
        int len = val.length();
        if (len > 0 && len < BitchatBLE::RX_BUF_SIZE && !_parent->_rx_ready) {
            memcpy((void *)_parent->_rx_buf, val.data(), len);
            _parent->_rx_len = len;
            _parent->_rx_ready = true;
        }
    }

private:
    BitchatBLE *_parent;
};

// ── BitchatBLE implementation ────────────────────────

bool BitchatBLE::begin() {
    Serial.printf("[%s] Initializing...\n", TAG);

    // Initialize bridge identity
    if (!bitchat_identity_init(&_keypair)) {
        Serial.printf("[%s] Failed to init identity\n", TAG);
        return false;
    }

    char fp_short[16];
    bitchat_fingerprint_short(_keypair.fingerprint, fp_short, sizeof(fp_short));
    Serial.printf("[%s] Bridge fingerprint: %s...\n", TAG, fp_short);

    _init_ble_server();
    _start_advertising();

    _active = true;
    Serial.printf("[%s] BLE active, advertising as %s\n", TAG, BRIDGE_NAME);
    return true;
}

void BitchatBLE::end() {
    if (_active) {
        NimBLEDevice::deinit(true);
        _active = false;
        _server = nullptr;
        _tx_char = nullptr;
        _rx_char = nullptr;
        Serial.printf("[%s] BLE stopped\n", TAG);
    }
}

bool BitchatBLE::is_active() const {
    return _active;
}

void BitchatBLE::loop() {
    if (!_active) return;

    // Process any incoming BLE message
    if (_rx_ready) {
        _process_incoming();
        _rx_ready = false;
    }

    // TODO: Periodically scan for other bitchat peers and connect to relay
    // _scan_for_peers();
}

bool BitchatBLE::send_text(const char *text) {
    if (!_active || !_tx_char) return false;

    int text_len = strlen(text);

    // Construct a BitchatPacket for broadcast over BLE NUS.
    //
    // Bitchat packet format:
    //   Fixed header (14 bytes):
    //     [version 1B][type 1B][TTL 1B][timestamp 8B][flags 1B][payload_len 2B]
    //   Variable fields:
    //     [sender_id 8B]  (always present — our bridge peer ID)
    //     [payload ...]   (UTF-8 text content)
    //
    // TODO Phase 2: Add Noise encryption, Ed25519 signature, PKCS#7 padding

    uint8_t packet[BITCHAT_HEADER_LEN + BITCHAT_SENDER_ID_LEN + 256];
    int pos = 0;

    // -- Fixed header (14 bytes) --
    // Version
    packet[pos++] = 0x01;
    // Type: 0x01 = broadcast text (message)
    packet[pos++] = 0x01;
    // TTL
    packet[pos++] = BITCHAT_MAX_HOPS;
    // Timestamp (8 bytes, milliseconds — use millis() as placeholder)
    uint64_t ts = (uint64_t)millis();
    for (int i = 7; i >= 0; i--) {
        packet[pos++] = (ts >> (i * 8)) & 0xFF;
    }
    // Flags: 0x00 = no recipient, no signature, not compressed
    packet[pos++] = 0x00;
    // Payload length (2 bytes, big-endian)
    int payload_len = (text_len < 220) ? text_len : 220;
    packet[pos++] = (payload_len >> 8) & 0xFF;
    packet[pos++] = payload_len & 0xFF;

    // -- Sender ID (8 bytes — first 8 bytes of our fingerprint) --
    memcpy(packet + pos, _keypair.fingerprint, BITCHAT_SENDER_ID_LEN);
    pos += BITCHAT_SENDER_ID_LEN;

    // -- Payload (UTF-8 text) --
    memcpy(packet + pos, text, payload_len);
    pos += payload_len;

    // Send via BLE notification
    _tx_char->setValue(packet, pos);
    _tx_char->notify();

    Serial.printf("[%s] Broadcast %d bytes over BLE\n", TAG, pos);
    return true;
}

// ── Private methods ──────────────────────────────────

void BitchatBLE::_init_ble_server() {
    NimBLEDevice::init(BRIDGE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Max transmit power

    _server = NimBLEDevice::createServer();

    // Create bitchat service
    NimBLEService *service = _server->createService(BITCHAT_SERVICE_UUID);

    // TX characteristic: bridge → remote peers (notify)
    _tx_char = service->createCharacteristic(
        BITCHAT_CHAR_TX_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // RX characteristic: remote peers → bridge (write)
    _rx_char = service->createCharacteristic(
        BITCHAT_CHAR_RX_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    _rx_char->setCallbacks(new BitchatBLECallbacks(this));

    service->start();
}

void BitchatBLE::_start_advertising() {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BITCHAT_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->start();
}

void BitchatBLE::_process_incoming() {
    // Parse the received BLE data as a bitchat packet.
    //
    // Format: [header 14B][sender_id 8B][payload ...][signature 64B if flagged]
    //
    // Header layout:
    //   [0]    version (1B)
    //   [1]    type (1B)
    //   [2]    TTL (1B)
    //   [3-10] timestamp (8B, ms)
    //   [11]   flags (1B): bit0=hasRecipient, bit1=hasSignature, bit2=isCompressed
    //   [12-13] payload_len (2B, big-endian)

    int min_size = BITCHAT_HEADER_LEN + BITCHAT_SENDER_ID_LEN;
    if (_rx_len < min_size) {
        Serial.printf("[%s] Received runt packet (%d bytes, need %d)\n", TAG, _rx_len, min_size);
        return;
    }

    const uint8_t *pkt = _rx_buf;

    // Parse header
    // uint8_t version = pkt[0];
    uint8_t type = pkt[1];
    // uint8_t ttl = pkt[2];
    // timestamp at pkt[3..10]
    uint8_t flags = pkt[11];
    uint16_t payload_len = (pkt[12] << 8) | pkt[13];

    // Calculate variable field offsets
    int offset = BITCHAT_HEADER_LEN;

    // Sender ID (always present)
    const uint8_t *sender_id = pkt + offset;
    offset += BITCHAT_SENDER_ID_LEN;

    // Recipient ID (8 bytes, if hasRecipient flag)
    if (flags & 0x01) {
        offset += 8; // skip recipient ID
    }

    // Payload starts here
    if (offset + payload_len > _rx_len) {
        Serial.printf("[%s] Truncated packet (need %d, have %d)\n", TAG, offset + payload_len, _rx_len);
        return;
    }

    // Only handle broadcast text for now
    if (type != 0x01) {
        Serial.printf("[%s] Ignoring packet type 0x%02x\n", TAG, type);
        return;
    }

    // Extract text
    BridgeMessage msg;
    msg.origin = MessageOrigin::BITCHAT;
    msg.timestamp_ms = millis();

    int copy_len = (payload_len < sizeof(msg.text) - 1) ? payload_len : sizeof(msg.text) - 1;
    memcpy(msg.text, pkt + offset, copy_len);
    msg.text[copy_len] = '\0';

    // Store sender's 8-byte peer ID in the first 8 bytes of the fingerprint field
    memcpy(msg.sender.bitchat_fingerprint, sender_id, BITCHAT_SENDER_ID_LEN);
    // Format short display name from peer ID
    snprintf(msg.sender.display_name, sizeof(msg.sender.display_name),
             "%02x%02x%02x%02x", sender_id[0], sender_id[1], sender_id[2], sender_id[3]);

    Serial.printf("[%s] Received from %s: \"%s\"\n", TAG, msg.sender.display_name, msg.text);

    if (_on_message) {
        _on_message(msg);
    }
}

void BitchatBLE::_scan_for_peers() {
    // TODO: Use NimBLEScan to find other bitchat devices and connect to
    // relay messages. This enables multi-hop mesh forwarding.
    //
    // Scan for devices advertising BITCHAT_SERVICE_UUID, connect, discover
    // characteristics, and register for notifications on their TX char.
}
