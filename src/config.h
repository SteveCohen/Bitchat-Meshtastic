#pragma once

// ── WiFi ──────────────────────────────────────────────
#define WIFI_SSID          "your-ssid"
#define WIFI_PASSWORD      "your-password"

// ── Meshtastic node (TCP interface) ───────────────────
#define MESHTASTIC_HOST    "192.168.1.100"
#define MESHTASTIC_PORT    4403

// ── Bridge behaviour ──────────────────────────────────
#define BRIDGE_NAME        "BitBridge"
#define MESHTASTIC_CHANNEL 0        // Primary channel index
#define MSG_PREFIX_MESH    "[M] "   // Prefix for messages originating from Meshtastic
#define MSG_PREFIX_BLE     "[B] "   // Prefix for messages originating from Bitchat BLE

// ── Meshtastic protocol constants ─────────────────────
#define MESH_START1        0x94
#define MESH_START2        0xC3
#define MESH_HEADER_LEN    4
#define MESH_MAX_PAYLOAD   512
#define MESH_BROADCAST     0xFFFFFFFF
#define MESH_HEARTBEAT_MS  (300 * 1000)  // 5 minutes
#define MESH_DATA_MAX      233           // Max bytes in Data.payload

// ── Meshtastic protobuf field tags / port numbers ─────
#define PORTNUM_TEXT_MESSAGE_APP  1

// ── Bitchat protocol constants ────────────────────────
#define BITCHAT_MAX_HOPS      7
#define BITCHAT_HEADER_LEN    14     // version(1)+type(1)+TTL(1)+timestamp(8)+flags(1)+payload_len(2)
#define BITCHAT_SENDER_ID_LEN 8     // 8-byte peer ID (truncated SHA-256 of Noise pubkey)
// Bitchat BLE uses a custom service with a SINGLE characteristic for both directions
// (write to send, notify to receive)
#define BITCHAT_SERVICE_UUID        "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C"
#define BITCHAT_MSG_CHAR_UUID       "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D"

// ── Bitchat packet types ──────────────────────────────
#define BITCHAT_PKT_MESSAGE         0x01
#define BITCHAT_PKT_DELIVERY_ACK    0x02
#define BITCHAT_PKT_READ_RECEIPT    0x03
#define BITCHAT_PKT_NOISE_HANDSHAKE 0x10
#define BITCHAT_PKT_NOISE_ENCRYPTED 0x11
#define BITCHAT_PKT_FRAGMENT        0x20

// ── Bitchat packet flags ──────────────────────────────
#define BITCHAT_FLAG_HAS_RECIPIENT  0x01
#define BITCHAT_FLAG_HAS_SIGNATURE  0x02
#define BITCHAT_FLAG_IS_COMPRESSED  0x04
#define BITCHAT_FLAG_IS_RELAY       0x08
#define BITCHAT_FLAG_IS_PRIVATE     0x10

// ── Bitchat TLV types (payload encoding) ──────────────
#define BITCHAT_TLV_NICKNAME        0x01
#define BITCHAT_TLV_NOISE_INIT      0x02
#define BITCHAT_TLV_NOISE_RESP      0x03
#define BITCHAT_TLV_NOISE_FINISH    0x04
#define BITCHAT_TLV_TEXT            0x05
#define BITCHAT_TLV_ENCRYPTED       0x06
#define BITCHAT_TLV_CHANNEL         0x07
#define BITCHAT_TLV_GEOHASH         0x08
#define BITCHAT_TLV_PUBKEY          0x09

// ── Bitchat BLE parameters ───────────────────────────
#define BITCHAT_MAX_CONNECTIONS     4
#define BITCHAT_SCAN_RSSI_MIN       (-70)
#define BITCHAT_BLE_MTU             512
#define BITCHAT_MAX_TEXT_LEN        100
#define BITCHAT_MSG_CACHE_SIZE      128
#define BITCHAT_MSG_CACHE_TTL_MS    (30 * 1000)

// ── Deduplication ─────────────────────────────────────
#define DEDUP_CACHE_SIZE   64       // Ring buffer of recent message hashes
#define DEDUP_TTL_MS       (60 * 1000)  // Ignore duplicates within 60 s
