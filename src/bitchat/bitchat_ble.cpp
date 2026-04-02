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

    // ── Phase 1: Send as raw UTF-8 over BLE notify ──────────
    //
    // TODO Phase 2: Properly construct a BitchatPacket with:
    //   - 13-byte header (version, type=BROADCAST, TTL=7, timestamp, flags, payload_len)
    //   - Noise encryption (or plaintext broadcast)
    //   - Bloom filter relay tracking
    //
    // For now we send plaintext so the bridge is functional for testing.
    // The bitchat app may need a compatibility shim to accept unencrypted
    // broadcast messages, OR we implement the full Noise handshake.

    // Construct a minimal bitchat-like broadcast packet
    // Header: [version=1][type=0x01 broadcast][TTL=7][timestamp 4B][flags=0][payload_len 2B]
    uint8_t packet[BITCHAT_HEADER_LEN + 256];
    int pos = 0;

    // Version
    packet[pos++] = 0x01;
    // Type: 0x01 = broadcast text
    packet[pos++] = 0x01;
    // TTL
    packet[pos++] = BITCHAT_MAX_HOPS;
    // Timestamp (4 bytes, seconds since epoch — use millis/1000 as placeholder)
    uint32_t ts = millis() / 1000;
    packet[pos++] = (ts >> 24) & 0xFF;
    packet[pos++] = (ts >> 16) & 0xFF;
    packet[pos++] = (ts >> 8) & 0xFF;
    packet[pos++] = ts & 0xFF;
    // Flags
    packet[pos++] = 0x00;
    // Reserved (3 bytes to reach 13-byte header)
    packet[pos++] = 0x00;
    packet[pos++] = 0x00;
    // Payload length (2 bytes, big-endian)
    int payload_len = (text_len < 240) ? text_len : 240;
    packet[pos++] = (payload_len >> 8) & 0xFF;
    packet[pos++] = payload_len & 0xFF;

    // Payload
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
    // Parse the received BLE data as a bitchat packet
    if (_rx_len < BITCHAT_HEADER_LEN) {
        Serial.printf("[%s] Received runt packet (%d bytes)\n", TAG, _rx_len);
        return;
    }

    const uint8_t *pkt = _rx_buf;

    // Parse header
    // uint8_t version = pkt[0];
    uint8_t type = pkt[1];
    // uint8_t ttl = pkt[2];
    // uint32_t timestamp = (pkt[3]<<24) | (pkt[4]<<16) | (pkt[5]<<8) | pkt[6];
    // uint8_t flags = pkt[7];
    uint16_t payload_len = (pkt[11] << 8) | pkt[12];

    if (BITCHAT_HEADER_LEN + payload_len > _rx_len) {
        Serial.printf("[%s] Truncated packet\n", TAG);
        return;
    }

    // Only handle broadcast text for now
    if (type != 0x01) {
        Serial.printf("[%s] Ignoring non-broadcast packet type 0x%02x\n", TAG, type);
        return;
    }

    // Extract text
    BridgeMessage msg;
    msg.origin = MessageOrigin::BITCHAT;
    msg.timestamp_ms = millis();

    int copy_len = (payload_len < sizeof(msg.text) - 1) ? payload_len : sizeof(msg.text) - 1;
    memcpy(msg.text, pkt + BITCHAT_HEADER_LEN, copy_len);
    msg.text[copy_len] = '\0';

    // TODO Phase 2: Extract sender fingerprint from Noise session or packet header
    // For now, sender is unknown
    snprintf(msg.sender.display_name, sizeof(msg.sender.display_name), "ble_peer");

    Serial.printf("[%s] Received: \"%s\"\n", TAG, msg.text);

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
