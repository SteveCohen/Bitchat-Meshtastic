#include "bitchat_ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <cstring>

static const char *TAG = "BitchatBLE";

// ── BLE Server Callbacks (peripheral role) ───────────────────────────

class BitchatBLEServerCallbacks : public NimBLEServerCallbacks {
public:
    BitchatBLEServerCallbacks(BitchatBLE *p) : _p(p) {}

    void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
        uint8_t addr_bytes[6];
        memcpy(addr_bytes, info.getAddress().getNative(), 6);
        // They connected to us → they are central, we are peripheral
        _p->_on_connect(info.getConnHandle(), addr_bytes, /*we_are_central=*/false);
        NimBLEDevice::startAdvertising();
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &info, int reason) override {
        _p->_on_disconnect(info.getConnHandle());
        NimBLEDevice::startAdvertising();
    }

private:
    BitchatBLE *_p;
};

// ── BLE Characteristic Write Callback (peripheral receives data) ─────

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

    _active = true;
    Serial.printf("[%s] Active, advertising as '%s'\n", TAG, BRIDGE_NAME);

    // Start scanning for other bitchat devices (central role)
    _start_scanning();

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

    // Process incoming packet from BLE callback
    if (_rx_ready) {
        _process_incoming();
        _rx_ready = false;
    }

    // Periodically restart scanning if we have room for more peers
    if (millis() - _last_scan_ms > 15000 && _active_peer_count() < BITCHAT_MAX_CONNECTIONS) {
        _start_scanning();
    }
}

// ── BLE server init (peripheral role) ────────────────────────────────

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

// ── BLE scanning (central role) ──────────────────────────────────────

// NimBLE 1.4 uses NimBLEAdvertisedDeviceCallbacks (not NimBLEScanCallbacks)
class BitchatBLEScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
    BitchatBLEScanCallbacks(BitchatBLE *p) : _p(p) {}

    void onResult(NimBLEAdvertisedDevice *dev) override {
        _p->_on_scan_result(dev);
    }

private:
    BitchatBLE *_p;
};

// Scan-complete callback (free function for NimBLE 1.4 API)
static BitchatBLE *_g_ble_instance = nullptr;
static void _scan_complete_cb(NimBLEScanResults results) {
    if (_g_ble_instance) _g_ble_instance->_scanning = false;
}

void BitchatBLE::_start_scanning() {
    if (_scanning) return;

    _g_ble_instance = this;  // for scan-complete callback

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(80);
    scan->setAdvertisedDeviceCallbacks(new BitchatBLEScanCallbacks(this), false);

    _scanning = true;
    _last_scan_ms = millis();
    scan->start(10, _scan_complete_cb, false);  // 10s, non-blocking with callback
    Serial.printf("[%s] Scanning for bitchat peers...\n", TAG);
}

void BitchatBLE::_on_scan_result(NimBLEAdvertisedDevice *dev) {
    // Only connect to devices advertising the bitchat service UUID
    if (!dev->isAdvertisingService(NimBLEUUID(BITCHAT_SERVICE_UUID))) return;

    // Check RSSI
    if (dev->getRSSI() < BITCHAT_SCAN_RSSI_MIN) return;

    // Check if already connected to this address
    NimBLEAddress addr = dev->getAddress();
    for (auto &p : _peers) {
        if (p.active && memcmp(p.ble_addr, addr.getNative(), 6) == 0) return;
    }

    // Check if we have room
    if (_active_peer_count() >= BITCHAT_MAX_CONNECTIONS) return;

    Serial.printf("[%s] Found bitchat peer: %s (RSSI %d)\n", TAG,
                  addr.toString().c_str(), dev->getRSSI());

    // Stop scanning before connecting
    NimBLEDevice::getScan()->stop();
    _scanning = false;

    _connect_to_peripheral(dev);
}

void BitchatBLE::_connect_to_peripheral(NimBLEAdvertisedDevice *dev) {
    NimBLEClient *client = NimBLEDevice::createClient();
    if (!client->connect(dev)) {
        Serial.printf("[%s] Failed to connect to %s\n", TAG, dev->getAddress().toString().c_str());
        NimBLEDevice::deleteClient(client);
        return;
    }

    // MTU is set globally via NimBLEDevice::setMTU() in _init_ble_server()

    // Discover the bitchat service and characteristic
    NimBLERemoteService *svc = client->getService(BITCHAT_SERVICE_UUID);
    if (!svc) {
        Serial.printf("[%s] Bitchat service not found on peer\n", TAG);
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return;
    }

    NimBLERemoteCharacteristic *chr = svc->getCharacteristic(BITCHAT_MSG_CHAR_UUID);
    if (!chr) {
        Serial.printf("[%s] Message characteristic not found on peer\n", TAG);
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return;
    }

    uint16_t handle = client->getConnId();
    uint8_t addr_bytes[6];
    memcpy(addr_bytes, dev->getAddress().getNative(), 6);

    // Register peer session BEFORE subscribing (so notify callback can find it)
    _on_connect(handle, addr_bytes, /*we_are_central=*/true);

    PeerSession *peer = _find_peer(handle);
    if (peer) {
        peer->client     = client;
        peer->remote_chr = chr;
    }

    // Subscribe to notifications (incoming packets from peer).
    // NimBLE 1.4 notify_callback: void(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool)
    if (chr->canNotify()) {
        chr->subscribe(true, [this](NimBLERemoteCharacteristic *c,
                                     uint8_t *data, size_t length, bool isNotify) {
            if (length > 0 && (int)length <= RX_BUF_SIZE && !_rx_ready) {
                memcpy((void *)_rx_buf, data, length);
                _rx_len         = (int)length;
                _rx_conn_handle = c->getRemoteService()->getClient()->getConnId();
                _rx_ready       = true;
            }
        });
    }

    Serial.printf("[%s] Connected to peripheral %s, handle=%d\n", TAG,
                  dev->getAddress().toString().c_str(), handle);
}

// ── send_text ─────────────────────────────────────────────────────────
// Sends a public message (PKT_MESSAGE = 0x02) to all connected peers.

bool BitchatBLE::send_text(const char *text) {
    if (!_active || !_msg_char) return false;

    int text_len = (int)strlen(text);
    if (text_len > BITCHAT_MAX_TEXT_LEN) text_len = BITCHAT_MAX_TEXT_LEN;

    // Build TLV payload with text
    uint8_t tlv_buf[256];
    int tlv_len = 0;
    tlv_len += _tlv_encode(tlv_buf + tlv_len, BITCHAT_TLV_TEXT,
                           (const uint8_t *)text, (uint8_t)text_len);

    _send_packet(BITCHAT_PKT_MESSAGE, 0x00, tlv_buf, (uint16_t)tlv_len);
    return true;
}

// ── _send_packet (broadcast) ──────────────────────────────────────────

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

    // Broadcast to all peers
    for (auto &p : _peers) {
        if (!p.active) continue;
        _send_to_peer(&p, packet, pos);
    }
}

// ── _send_to_peer (unicast) ──────────────────────────────────────────

void BitchatBLE::_send_to_peer(PeerSession *peer, const uint8_t *data, int len) {
    if (peer->we_are_central && peer->remote_chr) {
        // We are central: write to remote peripheral's characteristic (unicast)
        peer->remote_chr->writeValue(data, len, false);  // write without response
    } else if (_msg_char) {
        // We are peripheral: notify sends to ALL subscribed centrals.
        // NimBLE 1.4 has no per-connection notify — this is acceptable because:
        // - Handshake messages in wrong state are silently ignored by the peer
        // - The iOS app has one central connection per peripheral typically
        _msg_char->setValue(data, len);
        _msg_char->notify();
    }
}

// ── _send_announce ────────────────────────────────────────────────────
// Send type 0x01 with TLV: nickname + noisePublicKey + signingPublicKey

void BitchatBLE::_send_announce(PeerSession *peer) {
    uint8_t tlv_buf[128];
    int tlv_len = 0;

    const char *nick = BRIDGE_NAME;
    int nick_len = (int)strlen(nick);
    if (nick_len > 31) nick_len = 31;
    tlv_len += _tlv_encode(tlv_buf + tlv_len, BITCHAT_TLV_NICKNAME,
                           (const uint8_t *)nick, (uint8_t)nick_len);
    tlv_len += _tlv_encode(tlv_buf + tlv_len, BITCHAT_TLV_NOISE_PUBKEY,
                           _keypair.noise_public, 32);
    tlv_len += _tlv_encode(tlv_buf + tlv_len, BITCHAT_TLV_SIGNING_PUBKEY,
                           _keypair.sign_public, 32);  // zeroed for Phase 1

    // Build packet
    static uint8_t packet[512];
    int pos = 0;
    packet[pos++] = 0x01;                        // version
    packet[pos++] = BITCHAT_PKT_ANNOUNCE;
    packet[pos++] = BITCHAT_MAX_HOPS;
    uint64_t ts = (uint64_t)millis();
    for (int i = 7; i >= 0; i--) packet[pos++] = (ts >> (i * 8)) & 0xFF;
    packet[pos++] = 0x00;                        // flags
    packet[pos++] = ((uint16_t)tlv_len >> 8) & 0xFF;
    packet[pos++] =  (uint16_t)tlv_len       & 0xFF;
    memcpy(packet + pos, _keypair.fingerprint, BITCHAT_SENDER_ID_LEN);
    pos += BITCHAT_SENDER_ID_LEN;
    memcpy(packet + pos, tlv_buf, tlv_len);
    pos += tlv_len;
    pos = _pad_packet(packet, pos, sizeof(packet));

    _send_to_peer(peer, packet, pos);
    peer->announce_sent = true;
    Serial.printf("[%s] Sent announce to handle=%d\n", TAG, peer->conn_handle);
}

// ── _send_handshake_packet ────────────────────────────────────────────
// Sends raw Noise bytes in a PKT_NOISE_HANDSHAKE (0x10) unicast packet.

void BitchatBLE::_send_handshake_packet(PeerSession *peer,
                                         const uint8_t *payload, size_t payload_len) {
    static uint8_t packet[512];
    int pos = 0;

    packet[pos++] = 0x01;                        // version
    packet[pos++] = BITCHAT_PKT_NOISE_HANDSHAKE;
    packet[pos++] = BITCHAT_MAX_HOPS;
    uint64_t ts = (uint64_t)millis();
    for (int i = 7; i >= 0; i--) packet[pos++] = (ts >> (i * 8)) & 0xFF;
    packet[pos++] = 0x00;                        // flags
    packet[pos++] = ((uint16_t)payload_len >> 8) & 0xFF;
    packet[pos++] =  (uint16_t)payload_len       & 0xFF;
    memcpy(packet + pos, _keypair.fingerprint, BITCHAT_SENDER_ID_LEN);
    pos += BITCHAT_SENDER_ID_LEN;
    memcpy(packet + pos, payload, payload_len);
    pos += (int)payload_len;

    _send_to_peer(peer, packet, pos);
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
    uint8_t  ttl         = pkt[2];
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
        peer = _alloc_peer(_rx_conn_handle, nullptr, false);
    }

    // Update peer_id from sender_id if not yet known
    if (peer && !peer->peer_id_known) {
        memcpy(peer->peer_id, sender_id, BITCHAT_SENDER_ID_LEN);
        peer->peer_id_known = true;
    }

    switch (type) {
        case BITCHAT_PKT_ANNOUNCE:
            _handle_announce(peer, payload, payload_len, sender_id);
            break;
        case BITCHAT_PKT_MESSAGE:
            _handle_message(peer, payload, payload_len, sender_id, flags);
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

    // Relay: decrement TTL and forward to other peers (gossip)
    if (ttl > 1 && (type == BITCHAT_PKT_ANNOUNCE || type == BITCHAT_PKT_MESSAGE)) {
        _relay_packet(pkt, pkt_len, _rx_conn_handle);
    }
}

// ── _handle_announce ──────────────────────────────────────────────────

void BitchatBLE::_handle_announce(PeerSession *peer, const uint8_t *payload,
                                   int payload_len, const uint8_t *sender_id) {
    if (!peer) return;

    const uint8_t *nick_val = nullptr;
    uint8_t nick_len = 0;
    _tlv_find(payload, payload_len, BITCHAT_TLV_NICKNAME, &nick_val, &nick_len);

    const uint8_t *noise_pub = nullptr;
    uint8_t noise_pub_len = 0;
    _tlv_find(payload, payload_len, BITCHAT_TLV_NOISE_PUBKEY, &noise_pub, &noise_pub_len);

    char nick_str[33] = {};
    if (nick_val && nick_len > 0) {
        int n = (nick_len < 32) ? nick_len : 32;
        memcpy(nick_str, nick_val, n);
    }

    Serial.printf("[%s] Announce from %02x%02x%02x%02x nick='%s' noisePub=%s\n",
                  TAG, sender_id[0], sender_id[1], sender_id[2], sender_id[3],
                  nick_str, (noise_pub && noise_pub_len == 32) ? "yes" : "no");

    peer->announce_rcvd = true;

    // Send our announce back if we haven't yet
    if (!peer->announce_sent) {
        _send_announce(peer);
    }

    // If we are central (initiator), start the Noise handshake now
    if (peer->we_are_central && peer->hs.phase == NOISE_HS_IDLE) {
        if (noise_hs_init(&peer->hs, NOISE_ROLE_INITIATOR,
                          _keypair.noise_private, _keypair.noise_public) != 0) {
            Serial.printf("[%s] noise_hs_init failed\n", TAG);
            return;
        }
        uint8_t out[64]; size_t out_len = 0;
        if (noise_hs_write_msg1(&peer->hs, out, &out_len) == 0) {
            _send_handshake_packet(peer, out, out_len);
        }
    }
}

// ── _handle_message (public text, type 0x02) ─────────────────────────

void BitchatBLE::_handle_message(PeerSession * /*peer*/, const uint8_t *payload,
                                  int payload_len, const uint8_t *sender_id,
                                  uint8_t /*flags*/) {
    const uint8_t *text_val = nullptr;
    uint8_t text_len = 0;
    _tlv_find(payload, payload_len, BITCHAT_TLV_TEXT, &text_val, &text_len);

    if (!text_val || text_len == 0) return;

    BridgeMessage msg;
    msg.origin       = MessageOrigin::BITCHAT;
    msg.timestamp_ms = millis();

    int cl = (text_len < sizeof(msg.text) - 1) ? text_len : sizeof(msg.text) - 1;
    memcpy(msg.text, text_val, cl);
    msg.text[cl] = '\0';

    memcpy(msg.sender.bitchat_fingerprint, sender_id, BITCHAT_SENDER_ID_LEN);

    const uint8_t *nick_val = nullptr;
    uint8_t nick_len = 0;
    _tlv_find(payload, payload_len, BITCHAT_TLV_NICKNAME, &nick_val, &nick_len);

    if (nick_val && nick_len > 0) {
        int n = (nick_len < sizeof(msg.sender.display_name) - 1) ? nick_len : sizeof(msg.sender.display_name) - 1;
        memcpy(msg.sender.display_name, nick_val, n);
        msg.sender.display_name[n] = '\0';
    } else {
        snprintf(msg.sender.display_name, sizeof(msg.sender.display_name),
                 "%02x%02x%02x%02x", sender_id[0], sender_id[1], sender_id[2], sender_id[3]);
    }

    Serial.printf("[%s] Message from %s: \"%s\"\n", TAG, msg.sender.display_name, msg.text);

    if (_on_message) _on_message(msg);
}

// ── _handle_noise_handshake ──────────────────────────────────────────
// Payload is raw Noise bytes (no TLV wrapping).

void BitchatBLE::_handle_noise_handshake(PeerSession *peer,
                                          const uint8_t *payload, int payload_len) {
    if (!peer) return;

    uint8_t out[128]; size_t out_len = 0;
    int ret = 0;

    switch (peer->hs.phase) {
        case NOISE_HS_IDLE:
            // We are responder — this is message 1 (32 bytes)
            // Initialize as responder if not already
            if (noise_hs_init(&peer->hs, NOISE_ROLE_RESPONDER,
                              _keypair.noise_private, _keypair.noise_public) != 0) {
                Serial.printf("[%s] noise_hs_init (responder) failed\n", TAG);
                return;
            }
            Serial.printf("[%s] Received msg1 (%dB), responding...\n", TAG, payload_len);
            ret = noise_hs_read_msg1_write_msg2(&peer->hs, payload, payload_len,
                                                 out, &out_len);
            if (ret == 0) _send_handshake_packet(peer, out, out_len);
            break;

        case NOISE_HS_AWAIT_MSG2:
            // We are initiator — this is message 2 (80 bytes)
            Serial.printf("[%s] Received msg2 (%dB), sending msg3...\n", TAG, payload_len);
            ret = noise_hs_read_msg2_write_msg3(&peer->hs, payload, payload_len,
                                                 out, &out_len);
            if (ret == 0) _send_handshake_packet(peer, out, out_len);
            break;

        case NOISE_HS_AWAIT_MSG3:
            // We are responder — this is message 3 (48 bytes)
            Serial.printf("[%s] Received msg3 (%dB), finalising...\n", TAG, payload_len);
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
                                    const uint8_t *pkt, int /*pkt_len*/,
                                    const uint8_t *ciphertext, int ct_len) {
    if (!peer || peer->hs.phase != NOISE_HS_TRANSPORT) {
        Serial.printf("[%s] Encrypted message from peer without session\n", TAG);
        return;
    }

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

    // Decrypted payload is NoisePayload: type_byte + data
    if (pt_len < 1) return;
    uint8_t noise_type = plaintext[0];
    const uint8_t *inner = plaintext + 1;
    int inner_len = (int)pt_len - 1;

    if (noise_type == NOISE_PAYLOAD_PRIVATE_MSG && inner_len > 0) {
        // Inner data is a PrivateMessagePacket TLV: messageID(0x00) + content(0x01)
        // For bridge purposes, extract content TLV type 0x01
        const uint8_t *content_val = nullptr;
        uint8_t content_len = 0;
        _tlv_find(inner, inner_len, 0x01, &content_val, &content_len);

        if (content_val && content_len > 0) {
            BridgeMessage msg;
            msg.origin = MessageOrigin::BITCHAT;
            msg.timestamp_ms = millis();
            int cl = (content_len < sizeof(msg.text) - 1) ? content_len : sizeof(msg.text) - 1;
            memcpy(msg.text, content_val, cl);
            msg.text[cl] = '\0';
            memcpy(msg.sender.bitchat_fingerprint, peer->peer_id, BITCHAT_SENDER_ID_LEN);
            snprintf(msg.sender.display_name, sizeof(msg.sender.display_name),
                     "%02x%02x%02x%02x", peer->peer_id[0], peer->peer_id[1],
                     peer->peer_id[2], peer->peer_id[3]);

            Serial.printf("[%s] Private msg from %s: \"%s\"\n", TAG, msg.sender.display_name, msg.text);
            if (_on_message) _on_message(msg);
        }
    } else {
        Serial.printf("[%s] Encrypted payload type 0x%02x (%d bytes)\n", TAG, noise_type, inner_len);
    }
}

// ── _relay_packet ─────────────────────────────────────────────────────
// Forward a received packet to all other connected peers with TTL - 1.

void BitchatBLE::_relay_packet(const uint8_t *pkt, int pkt_len, uint16_t except_handle) {
    if (pkt_len < BITCHAT_HEADER_LEN) return;

    // Copy packet and decrement TTL
    static uint8_t relay_buf[2048];
    int copy_len = (pkt_len < (int)sizeof(relay_buf)) ? pkt_len : (int)sizeof(relay_buf);
    memcpy(relay_buf, pkt, copy_len);
    relay_buf[2] = relay_buf[2] - 1;  // decrement TTL

    // Re-pad
    copy_len = _pad_packet(relay_buf, copy_len, sizeof(relay_buf));

    for (auto &p : _peers) {
        if (!p.active || p.conn_handle == except_handle) continue;
        _send_to_peer(&p, relay_buf, copy_len);
    }
}

// ── Peer management ───────────────────────────────────────────────────

PeerSession *BitchatBLE::_find_peer(uint16_t handle) {
    for (auto &p : _peers) {
        if (p.active && p.conn_handle == handle) return &p;
    }
    return nullptr;
}

PeerSession *BitchatBLE::_alloc_peer(uint16_t handle, const uint8_t *addr, bool we_are_central) {
    for (auto &p : _peers) {
        if (!p.active) {
            memset(&p, 0, sizeof(PeerSession));
            p.active         = true;
            p.conn_handle    = handle;
            p.we_are_central = we_are_central;
            if (addr) memcpy(p.ble_addr, addr, 6);
            return &p;
        }
    }
    return nullptr; // full
}

void BitchatBLE::_free_peer(uint16_t handle) {
    for (auto &p : _peers) {
        if (p.active && p.conn_handle == handle) {
            if (p.client) {
                p.client->disconnect();
                NimBLEDevice::deleteClient(p.client);
            }
            memset(&p, 0, sizeof(PeerSession));
            return;
        }
    }
}

int BitchatBLE::_active_peer_count() const {
    int count = 0;
    for (const auto &p : _peers) {
        if (p.active) count++;
    }
    return count;
}

// ── Connection / disconnection ────────────────────────────────────────

void BitchatBLE::_on_connect(uint16_t handle, const uint8_t *addr, bool we_are_central) {
    Serial.printf("[%s] Peer connected, handle=%d, role=%s, addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
                  TAG, handle, we_are_central ? "central" : "peripheral",
                  addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    PeerSession *peer = _alloc_peer(handle, addr, we_are_central);
    if (!peer) {
        Serial.printf("[%s] No free peer slots\n", TAG);
        return;
    }

    // Send our announce immediately
    _send_announce(peer);

    // Role determination: Central = initiator, Peripheral = responder
    // The handshake starts after we receive the peer's announce (which contains
    // their identity). See _handle_announce() for the initiator path.
    // The responder path starts in _handle_noise_handshake() when msg1 arrives.
}

void BitchatBLE::_on_disconnect(uint16_t handle) {
    Serial.printf("[%s] Peer disconnected, handle=%d\n", TAG, handle);
    _free_peer(handle);
}
