#include "bitchat_ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

static const char *TAG = "BitchatBLE";

// ── BLE Write Callback ───────────────────────────────
// Called when a remote bitchat device writes to our message characteristic.

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

// ── TLV helpers ──────────────────────────────────────

int BitchatBLE::_tlv_encode(uint8_t *buf, uint8_t type, const uint8_t *value, uint8_t len) {
    buf[0] = type;
    buf[1] = len;
    memcpy(buf + 2, value, len);
    return 2 + len;
}

bool BitchatBLE::_tlv_find(const uint8_t *buf, int buf_len, uint8_t type,
                            const uint8_t **value, uint8_t *len) {
    int pos = 0;
    while (pos + 2 <= buf_len) {
        uint8_t t = buf[pos];
        uint8_t l = buf[pos + 1];
        if (pos + 2 + l > buf_len) break;
        if (t == type) {
            *value = buf + pos + 2;
            *len = l;
            return true;
        }
        pos += 2 + l;
    }
    return false;
}

// ── PKCS#7 padding ───────────────────────────────────

int BitchatBLE::_pad_packet(uint8_t *buf, int data_len, int buf_size) {
    // Find target block size
    int target;
    if (data_len <= 256) target = 256;
    else if (data_len <= 512) target = 512;
    else if (data_len <= 1024) target = 1024;
    else target = 2048;

    if (target > buf_size) target = buf_size;

    int pad_len = target - data_len;
    if (pad_len <= 0) return data_len;

    memset(buf + data_len, (uint8_t)pad_len, pad_len);
    return target;
}

// ── Unpad PKCS#7 ─────────────────────────────────────

static int unpad_packet(const uint8_t *buf, int buf_len) {
    if (buf_len <= 0) return 0;
    uint8_t pad_val = buf[buf_len - 1];
    if (pad_val == 0 || pad_val > buf_len) return buf_len; // not padded
    // Verify padding bytes
    for (int i = buf_len - pad_val; i < buf_len; i++) {
        if (buf[i] != pad_val) return buf_len; // invalid padding
    }
    return buf_len - pad_val;
}

// ── BitchatBLE implementation ────────────────────────

bool BitchatBLE::begin() {
    Serial.printf("[%s] Initializing...\n", TAG);

    if (!bitchat_identity_init(&_keypair)) {
        Serial.printf("[%s] Failed to init identity\n", TAG);
        return false;
    }

    char fp_short[16];
    bitchat_fingerprint_short(_keypair.fingerprint, fp_short, sizeof(fp_short));
    Serial.printf("[%s] Bridge peer ID: %s...\n", TAG, fp_short);

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
        _msg_char = nullptr;
        Serial.printf("[%s] BLE stopped\n", TAG);
    }
}

bool BitchatBLE::is_active() const {
    return _active;
}

void BitchatBLE::loop() {
    if (!_active) return;

    if (_rx_ready) {
        _process_incoming();
        _rx_ready = false;
    }

    // TODO: Periodically scan for other bitchat peers and connect to relay
    // _scan_for_peers();
}

bool BitchatBLE::send_text(const char *text) {
    if (!_active || !_msg_char) return false;

    int text_len = strlen(text);
    if (text_len > BITCHAT_MAX_TEXT_LEN) text_len = BITCHAT_MAX_TEXT_LEN;

    // ── Build BitchatPacket ──────────────────────────────
    //
    // Format:
    //   [header 14B][sender_id 8B][TLV payload ...][PKCS#7 padding]
    //
    // TLV payload for a broadcast text message:
    //   [TLV_NICKNAME][TLV_TEXT]

    // Static buffer — avoids 2048-byte stack allocation on BLE task
    static uint8_t packet[2048];
    int pos = 0;

    // -- Fixed header (14 bytes) --
    packet[pos++] = 0x01;                       // version
    packet[pos++] = BITCHAT_PKT_MESSAGE;        // type
    packet[pos++] = BITCHAT_MAX_HOPS;           // TTL

    // Timestamp (8 bytes big-endian, milliseconds)
    uint64_t ts = (uint64_t)millis();
    for (int i = 7; i >= 0; i--) {
        packet[pos++] = (ts >> (i * 8)) & 0xFF;
    }

    packet[pos++] = 0x00;                       // flags: broadcast, no sig, no compression

    // -- Payload: build TLV into a temp buffer, then write length + TLVs --
    uint8_t tlv_buf[256];
    int tlv_len = 0;

    // TLV_NICKNAME
    const char *nick = BRIDGE_NAME;
    int nick_len = strlen(nick);
    if (nick_len > 31) nick_len = 31;
    tlv_len += _tlv_encode(tlv_buf + tlv_len, BITCHAT_TLV_NICKNAME,
                           (const uint8_t *)nick, nick_len);

    // TLV_TEXT
    tlv_len += _tlv_encode(tlv_buf + tlv_len, BITCHAT_TLV_TEXT,
                           (const uint8_t *)text, text_len);

    // Payload length (2 bytes big-endian)
    packet[pos++] = (tlv_len >> 8) & 0xFF;
    packet[pos++] = tlv_len & 0xFF;

    // -- Sender ID (8 bytes = first 8 bytes of fingerprint) --
    memcpy(packet + pos, _keypair.fingerprint, BITCHAT_SENDER_ID_LEN);
    pos += BITCHAT_SENDER_ID_LEN;

    // -- TLV payload --
    memcpy(packet + pos, tlv_buf, tlv_len);
    pos += tlv_len;

    // -- PKCS#7 padding --
    pos = _pad_packet(packet, pos, sizeof(packet));

    // Send via BLE notification on the single message characteristic
    _msg_char->setValue(packet, pos);
    _msg_char->notify();

    Serial.printf("[%s] Broadcast %d bytes (padded) over BLE\n", TAG, pos);
    return true;
}

// ── Private methods ──────────────────────────────────

void BitchatBLE::_init_ble_server() {
    NimBLEDevice::init(BRIDGE_NAME);
    NimBLEDevice::setPower(9);   // +9 dBm — max for ESP32-S3
    NimBLEDevice::setMTU(BITCHAT_BLE_MTU);

    _server = NimBLEDevice::createServer();

    NimBLEService *service = _server->createService(BITCHAT_SERVICE_UUID);

    // Single characteristic for both directions: write to send, notify to receive
    _msg_char = service->createCharacteristic(
        BITCHAT_MSG_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY
    );
    _msg_char->setCallbacks(new BitchatBLECallbacks(this));

    service->start();
}

void BitchatBLE::_start_advertising() {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BITCHAT_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->start();
}

void BitchatBLE::_process_incoming() {
    // Remove PKCS#7 padding first
    int pkt_len = unpad_packet(_rx_buf, _rx_len);

    // Minimum: 14-byte header + 8-byte sender_id
    int min_size = BITCHAT_HEADER_LEN + BITCHAT_SENDER_ID_LEN;
    if (pkt_len < min_size) {
        Serial.printf("[%s] Runt packet (%d bytes after unpad)\n", TAG, pkt_len);
        return;
    }

    const uint8_t *pkt = _rx_buf;

    // -- Parse header --
    // uint8_t version = pkt[0];
    uint8_t type = pkt[1];
    // uint8_t ttl = pkt[2];
    // timestamp at pkt[3..10]
    uint8_t flags = pkt[11];
    uint16_t payload_len = (pkt[12] << 8) | pkt[13];

    // -- Variable fields --
    int offset = BITCHAT_HEADER_LEN;

    // Sender ID (always present)
    const uint8_t *sender_id = pkt + offset;
    offset += BITCHAT_SENDER_ID_LEN;

    // Optional recipient ID
    if (flags & BITCHAT_FLAG_HAS_RECIPIENT) {
        offset += 8;
    }

    // Payload
    if (offset + payload_len > pkt_len) {
        Serial.printf("[%s] Truncated payload\n", TAG);
        return;
    }

    const uint8_t *payload = pkt + offset;

    // Only handle plaintext message packets for now
    if (type != BITCHAT_PKT_MESSAGE) {
        Serial.printf("[%s] Ignoring packet type 0x%02x\n", TAG, type);
        return;
    }

    // Parse TLV payload to extract nickname and text
    const uint8_t *nick_val = nullptr;
    uint8_t nick_len = 0;
    const uint8_t *text_val = nullptr;
    uint8_t text_len = 0;

    _tlv_find(payload, payload_len, BITCHAT_TLV_NICKNAME, &nick_val, &nick_len);
    _tlv_find(payload, payload_len, BITCHAT_TLV_TEXT, &text_val, &text_len);

    if (!text_val || text_len == 0) {
        Serial.printf("[%s] No TLV_TEXT in message\n", TAG);
        return;
    }

    // Build BridgeMessage
    BridgeMessage msg;
    msg.origin = MessageOrigin::BITCHAT;
    msg.timestamp_ms = millis();

    // Copy text
    int copy_len = (text_len < sizeof(msg.text) - 1) ? text_len : sizeof(msg.text) - 1;
    memcpy(msg.text, text_val, copy_len);
    msg.text[copy_len] = '\0';

    // Store sender peer ID
    memcpy(msg.sender.bitchat_fingerprint, sender_id, BITCHAT_SENDER_ID_LEN);

    // Use nickname if available, otherwise format from peer ID
    if (nick_val && nick_len > 0) {
        int n = (nick_len < sizeof(msg.sender.display_name) - 1) ? nick_len : sizeof(msg.sender.display_name) - 1;
        memcpy(msg.sender.display_name, nick_val, n);
        msg.sender.display_name[n] = '\0';
    } else {
        snprintf(msg.sender.display_name, sizeof(msg.sender.display_name),
                 "%02x%02x%02x%02x", sender_id[0], sender_id[1], sender_id[2], sender_id[3]);
    }

    Serial.printf("[%s] From %s: \"%s\"\n", TAG, msg.sender.display_name, msg.text);

    if (_on_message) {
        _on_message(msg);
    }
}

void BitchatBLE::_scan_for_peers() {
    // TODO: Use NimBLEScan to find other bitchat devices advertising
    // BITCHAT_SERVICE_UUID, connect (lower BLE address initiates),
    // discover BITCHAT_MSG_CHAR_UUID, subscribe to notifications.
    // RSSI threshold: BITCHAT_SCAN_RSSI_MIN (-70 dBm)
    // Max connections: BITCHAT_MAX_CONNECTIONS (4)
    // Connection race: compare BLE addresses, lower address initiates.
    // Rate limit: 5s cooldown between attempts.
}
