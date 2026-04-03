#include "bitchat_ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <cstring>

static const char *TAG = "BitchatBLE";

// ── BLE Server Callbacks ──────────────────────────────────────────────
// Track connect/disconnect to manage per-peer Noise sessions.

class BitchatBLEServerCallbacks : public NimBLEServerCallbacks {
public:
    BitchatBLEServerCallbacks(BitchatBLE *p) : _p(p) {}

    void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
        const NimBLEAddress &addr = info.getAddress();
        // NimBLE stores address as 6 bytes; copy to local buffer
        uint8_t addr_bytes[6];
        memcpy(addr_bytes, addr.getNative(), 6);
        _p->_on_connect(info.getConnHandle(), addr_bytes);
        // Restart advertising so other devices can still connect
        NimBLEDevice::startAdvertising();
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &info, int reason) override {
        _p->_on_disconnect(info.getConnHandle());
        NimBLEDevice::startAdvertising();
    }

private:
    BitchatBLE *_p;
};

// ── BLE Characteristic Write Callback ────────────────────────────────

class BitchatBLECallbacks : public NimBLECharacteristicCallbacks {
public:
    BitchatBLECallbacks(BitchatBLE *p) : _p(p) {}

    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
        NimBLEAttValue val = chr->getValue();
        int len = (int)val.length();
        if (len > 0 && len <= BitchatBLE::RX_BUF_SIZE && !_p->_rx_ready) {
            memcpy((void *)_p->_rx_buf, val.data(), len);
            _p->_rx_len         = len;
            _p->_rx_conn_handle = info.getConnHandle();
            _p->_rx_ready       = true;
        }
    }

private:
    BitchatBLE *_p;
};

// ── TLV helpers (static) ──────────────────────────────────────────────

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
        uint8_t t = buf[pos], l = buf[pos + 1];
        if (pos + 2 + l > buf_len) break;
        if (t == type) { *value = buf + pos + 2; *len = l; return true; }
        pos += 2 + l;
    }
    return false;
}

// ── PKCS#7 padding ────────────────────────────────────────────────────

int BitchatBLE::_pad_packet(uint8_t *buf, int data_len, int buf_size) {
    int target;
    if      (data_len <= 256)  target = 256;
    else if (data_len <= 512)  target = 512;
    else if (data_len <= 1024) target = 1024;
    else                       target = 2048;
    if (target > buf_size) target = buf_size;
    int pad_len = target - data_len;
    if (pad_len <= 0) return data_len;
    memset(buf + data_len, (uint8_t)pad_len, pad_len);
    return target;
}

static int unpad_packet(const uint8_t *buf, int len) {
    if (len <= 0) return 0;
    uint8_t pv = buf[len - 1];
    if (pv == 0 || pv > len) return len;
    for (int i = len - pv; i < len; i++)
        if (buf[i] != pv) return len;
    return len - pv;
}

// ── begin / end / is_active / loop ───────────────────────────────────

bool BitchatBLE::begin() {
    Serial.printf("[%s] Initializing...\n", TAG);

    if (!bitchat_identity_init(&_keypair)) {
        Serial.printf("[%s] Failed to init identity\n", TAG);
        return false;
    }

    char fp[16];
    bitchat_fingerprint_short(_keypair.fingerprint, fp, sizeof(fp));
    Serial.printf("[%s] Peer ID: %s...\n", TAG, fp);

    _init_ble_server();
    _start_advertising();

    // Capture our own BLE address for handshake role determination
    NimBLEAddress own = NimBLEDevice::getAddress();
    memcpy(_our_addr, own.getNative(), 6);

    _active = true;
    Serial.printf("[%s] Active, advertising as '%s'\n", TAG, BRIDGE_NAME);
    return true;
}

void BitchatBLE::end() {
    if (_active) {
        NimBLEDevice::deinit(true);
        _active    = false;
        _server    = nullptr;
        _msg_char  = nullptr;
        Serial.printf("[%s] Stopped\n", TAG);
    }
}

bool BitchatBLE::is_active() const { return _active; }

void BitchatBLE::loop() {
    if (!_active) return;
    if (_rx_ready) {
        _process_incoming();
        _rx_ready = false;
    }
}

// ── send_text ─────────────────────────────────────────────────────────
// Sends a plaintext broadcast (PKT_MESSAGE) to all connected peers.
// Peers that have completed the Noise handshake will receive the message.

bool BitchatBLE::send_text(const char *text) {
    if (!_active || !_msg_char) return false;

    int text_len = (int)strlen(text);
    if (text_len > BITCHAT_MAX_TEXT_LEN) text_len = BITCHAT_MAX_TEXT_LEN;

    // Build TLV payload
    uint8_t tlv_buf[256];
    int tlv_len = 0;
    const char *nick = BRIDGE_NAME;
    int nick_len = (int)strlen(nick);
    if (nick_len > 31) nick_len = 31;
    tlv_len += _tlv_encode(tlv_buf + tlv_len, BITCHAT_TLV_NICKNAME,
                           (const uint8_t *)nick, (uint8_t)nick_len);
    tlv_len += _tlv_encode(tlv_buf + tlv_len, BITCHAT_TLV_TEXT,
                           (const uint8_t *)text, (uint8_t)text_len);

    _send_packet(BITCHAT_PKT_MESSAGE, 0x00, tlv_buf, (uint16_t)tlv_len);
    return true;
}

// ── _send_packet ──────────────────────────────────────────────────────

void BitchatBLE::_send_packet(uint8_t type, uint8_t flags,
                               const uint8_t *payload, uint16_t payload_len) {
    static uint8_t packet[2048];
    int pos = 0;

    // Fixed header (14 bytes)
    packet[pos++] = 0x01;                        // version
    packet[pos++] = type;
    packet[pos++] = BITCHAT_MAX_HOPS;            // TTL
    uint64_t ts = (uint64_t)millis();
    for (int i = 7; i >= 0; i--) packet[pos++] = (ts >> (i * 8)) & 0xFF;
    packet[pos++] = flags;
    packet[pos++] = (payload_len >> 8) & 0xFF;
    packet[pos++] =  payload_len       & 0xFF;

    // Sender ID = first 8 bytes of our fingerprint
    memcpy(packet + pos, _keypair.fingerprint, BITCHAT_SENDER_ID_LEN);
    pos += BITCHAT_SENDER_ID_LEN;

    // Payload
    if (payload && payload_len > 0) {
        memcpy(packet + pos, payload, payload_len);
        pos += payload_len;
    }

    pos = _pad_packet(packet, pos, sizeof(packet));

    _msg_char->setValue(packet, pos);
    _msg_char->notify();
}

void BitchatBLE::_send_handshake_packet(uint16_t conn_handle,
                                         const uint8_t *tlv_payload, size_t tlv_len) {
    static uint8_t packet[512];
    int pos = 0;

    packet[pos++] = 0x01;                        // version
    packet[pos++] = BITCHAT_PKT_NOISE_HANDSHAKE;
    packet[pos++] = BITCHAT_MAX_HOPS;
    uint64_t ts = (uint64_t)millis();
    for (int i = 7; i >= 0; i--) packet[pos++] = (ts >> (i * 8)) & 0xFF;
    packet[pos++] = 0x00;                        // flags
    packet[pos++] = ((uint16_t)tlv_len >> 8) & 0xFF;
    packet[pos++] =  (uint16_t)tlv_len       & 0xFF;
    memcpy(packet + pos, _keypair.fingerprint, BITCHAT_SENDER_ID_LEN);
    pos += BITCHAT_SENDER_ID_LEN;
    memcpy(packet + pos, tlv_payload, tlv_len);
    pos += (int)tlv_len;

    // Send only to the specific peer — handshake messages are unicast
    _msg_char->setValue(packet, pos);
    _msg_char->notify(conn_handle);
}

// ── _process_incoming ─────────────────────────────────────────────────

void BitchatBLE::_process_incoming() {
    int pkt_len = unpad_packet(_rx_buf, _rx_len);
    int min_sz  = BITCHAT_HEADER_LEN + BITCHAT_SENDER_ID_LEN;

    if (pkt_len < min_sz) {
        Serial.printf("[%s] Runt packet (%d B)\n", TAG, pkt_len);
        return;
    }

    const uint8_t *pkt = _rx_buf;
    uint8_t  type        = pkt[1];
    uint8_t  flags       = pkt[11];
    uint16_t payload_len = (pkt[12] << 8) | pkt[13];

    int offset = BITCHAT_HEADER_LEN;
    const uint8_t *sender_id = pkt + offset;
    offset += BITCHAT_SENDER_ID_LEN;

    if (flags & BITCHAT_FLAG_HAS_RECIPIENT) offset += 8; // skip recipient

    if (offset + payload_len > pkt_len) {
        Serial.printf("[%s] Truncated payload\n", TAG);
        return;
    }

    const uint8_t *payload = pkt + offset;

    // Find (or lazily create) peer session
    PeerSession *peer = _find_peer(_rx_conn_handle);
    if (!peer) {
        // Connection must have been missed; create without address
        peer = _alloc_peer(_rx_conn_handle, nullptr);
    }

    // Update peer_id from sender_id if not yet known
    if (peer && !peer->peer_id_known) {
        memcpy(peer->peer_id, sender_id, BITCHAT_SENDER_ID_LEN);
        peer->peer_id_known = true;
    }

    switch (type) {
        case BITCHAT_PKT_MESSAGE:
            _handle_plaintext(peer, payload, payload_len, sender_id, flags);
            break;
        case BITCHAT_PKT_NOISE_HANDSHAKE:
            _handle_noise_handshake(peer, payload, payload_len);
            break;
        case BITCHAT_PKT_NOISE_ENCRYPTED:
            _handle_encrypted(peer, pkt, min_sz, payload, payload_len);
            break;
        default:
            Serial.printf("[%s] Unknown packet type 0x%02x\n", TAG, type);
            break;
    }
}

// ── _handle_plaintext ─────────────────────────────────────────────────

void BitchatBLE::_handle_plaintext(PeerSession * /*peer*/, const uint8_t *payload,
                                    int payload_len, const uint8_t *sender_id,
                                    uint8_t /*flags*/) {
    const uint8_t *nick_val = nullptr, *text_val = nullptr;
    uint8_t nick_len = 0, text_len = 0;
    _tlv_find(payload, payload_len, BITCHAT_TLV_NICKNAME, &nick_val, &nick_len);
    _tlv_find(payload, payload_len, BITCHAT_TLV_TEXT,     &text_val, &text_len);

    if (!text_val || text_len == 0) return;

    BridgeMessage msg;
    msg.origin       = MessageOrigin::BITCHAT;
    msg.timestamp_ms = millis();

    int cl = (text_len < sizeof(msg.text) - 1) ? text_len : sizeof(msg.text) - 1;
    memcpy(msg.text, text_val, cl);
    msg.text[cl] = '\0';

    memcpy(msg.sender.bitchat_fingerprint, sender_id, BITCHAT_SENDER_ID_LEN);

    if (nick_val && nick_len > 0) {
        int n = (nick_len < sizeof(msg.sender.display_name) - 1) ? nick_len : sizeof(msg.sender.display_name) - 1;
        memcpy(msg.sender.display_name, nick_val, n);
        msg.sender.display_name[n] = '\0';
    } else {
        snprintf(msg.sender.display_name, sizeof(msg.sender.display_name),
                 "%02x%02x%02x%02x", sender_id[0], sender_id[1], sender_id[2], sender_id[3]);
    }

    Serial.printf("[%s] Plaintext from %s: \"%s\"\n", TAG, msg.sender.display_name, msg.text);

    if (_on_message) _on_message(msg);
}

// ── _handle_noise_handshake ───────────────────────────────────────────

void BitchatBLE::_handle_noise_handshake(PeerSession *peer,
                                          const uint8_t *payload, int payload_len) {
    if (!peer) return;

    uint8_t out[256]; size_t out_len = 0;
    int ret = 0;

    switch (peer->hs.phase) {
        case NOISE_HS_IDLE:
            // We are the responder — this is message 1
            Serial.printf("[%s] Received msg1, responding...\n", TAG);
            ret = noise_hs_read_msg1_write_msg2(&peer->hs, payload, payload_len,
                                                 out, &out_len);
            if (ret == 0) _send_handshake_packet(peer->conn_handle, out, out_len);
            break;

        case NOISE_HS_AWAIT_MSG2:
            // We are the initiator — this is message 2
            Serial.printf("[%s] Received msg2, sending msg3...\n", TAG);
            ret = noise_hs_read_msg2_write_msg3(&peer->hs, payload, payload_len,
                                                 out, &out_len);
            if (ret == 0) _send_handshake_packet(peer->conn_handle, out, out_len);
            break;

        case NOISE_HS_AWAIT_MSG3:
            // We are the responder — this is message 3
            Serial.printf("[%s] Received msg3, finalising...\n", TAG);
            ret = noise_hs_read_msg3(&peer->hs, payload, payload_len);
            break;

        case NOISE_HS_TRANSPORT:
            Serial.printf("[%s] Unexpected handshake packet (already in transport)\n", TAG);
            break;

        default:
            break;
    }

    if (ret != 0) {
        Serial.printf("[%s] Handshake step failed (%d)\n", TAG, ret);
    } else if (peer->hs.phase == NOISE_HS_TRANSPORT) {
        Serial.printf("[%s] Noise session established with peer %02x%02x%02x%02x...\n",
                      TAG, peer->peer_id[0], peer->peer_id[1],
                           peer->peer_id[2], peer->peer_id[3]);
    }
}

// ── _handle_encrypted ────────────────────────────────────────────────

void BitchatBLE::_handle_encrypted(PeerSession *peer,
                                    const uint8_t *pkt, int hdr_len,
                                    const uint8_t *ciphertext, int ct_len) {
    if (!peer || peer->hs.phase != NOISE_HS_TRANSPORT) {
        Serial.printf("[%s] Encrypted message from peer without session\n", TAG);
        return;
    }

    // Use the 14-byte packet header as additional data (AAD)
    static uint8_t plaintext[512];
    size_t pt_len = 0;
    int ret = noise_hs_decrypt(&peer->hs,
                                pkt, BITCHAT_HEADER_LEN,  // AAD = header
                                ciphertext, ct_len,
                                plaintext, &pt_len);
    if (ret != 0) {
        Serial.printf("[%s] Decrypt failed (%d)\n", TAG, ret);
        return;
    }

    // Parse inner TLVs — same format as plaintext messages
    _handle_plaintext(peer, plaintext, (int)pt_len,
                      peer->peer_id, 0);
}

// ── Peer management ───────────────────────────────────────────────────

PeerSession *BitchatBLE::_find_peer(uint16_t handle) {
    for (auto &p : _peers) {
        if (p.active && p.conn_handle == handle) return &p;
    }
    return nullptr;
}

PeerSession *BitchatBLE::_alloc_peer(uint16_t handle, const uint8_t *addr) {
    for (auto &p : _peers) {
        if (!p.active) {
            memset(&p, 0, sizeof(PeerSession));
            p.active      = true;
            p.conn_handle = handle;
            if (addr) memcpy(p.ble_addr, addr, 6);
            return &p;
        }
    }
    return nullptr; // full
}

void BitchatBLE::_free_peer(uint16_t handle) {
    for (auto &p : _peers) {
        if (p.active && p.conn_handle == handle) {
            memset(&p, 0, sizeof(PeerSession));
            return;
        }
    }
}

// ── Connection / disconnection ────────────────────────────────────────

void BitchatBLE::_on_connect(uint16_t handle, const uint8_t *addr) {
    Serial.printf("[%s] Peer connected, handle=%d, addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
                  TAG, handle,
                  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    PeerSession *peer = _alloc_peer(handle, addr);
    if (!peer) {
        Serial.printf("[%s] No free peer slots\n", TAG);
        return;
    }

    // Determine handshake role: lower BLE address = initiator.
    // Addresses are stored little-endian; compare byte-by-byte from MSB (index 5→0).
    bool we_initiate = false;
    for (int i = 5; i >= 0; i--) {
        if (_our_addr[i] < addr[i]) { we_initiate = true;  break; }
        if (_our_addr[i] > addr[i]) { we_initiate = false; break; }
    }

    Serial.printf("[%s] We are %s\n", TAG, we_initiate ? "initiator" : "responder");

    NoiseRole role = we_initiate ? NOISE_ROLE_INITIATOR : NOISE_ROLE_RESPONDER;
    if (noise_hs_init(&peer->hs, role, _keypair.noise_private, _keypair.noise_public) != 0) {
        Serial.printf("[%s] noise_hs_init failed\n", TAG);
        return;
    }

    if (we_initiate) {
        uint8_t out[256]; size_t out_len = 0;
        if (noise_hs_write_msg1(&peer->hs, out, &out_len) == 0) {
            _send_handshake_packet(handle, out, out_len);
        }
    }
}

void BitchatBLE::_on_disconnect(uint16_t handle) {
    Serial.printf("[%s] Peer disconnected, handle=%d\n", TAG, handle);
    _free_peer(handle);
}

// ── BLE server init & advertising ────────────────────────────────────

void BitchatBLE::_init_ble_server() {
    NimBLEDevice::init(BRIDGE_NAME);
    NimBLEDevice::setPower(9);
    NimBLEDevice::setMTU(BITCHAT_BLE_MTU);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new BitchatBLEServerCallbacks(this));

    NimBLEService *svc = _server->createService(BITCHAT_SERVICE_UUID);

    _msg_char = svc->createCharacteristic(
        BITCHAT_MSG_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY
    );
    _msg_char->setCallbacks(new BitchatBLECallbacks(this));

    svc->start();
}

void BitchatBLE::_start_advertising() {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BITCHAT_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->start();
}
