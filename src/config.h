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
// Bitchat uses Nordic UART Service (NUS) over BLE
#define BITCHAT_SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"  // NUS Service
#define BITCHAT_CHAR_RX_UUID        "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // NUS RX (write to device)
#define BITCHAT_CHAR_TX_UUID        "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // NUS TX (notify from device)

// ── Deduplication ─────────────────────────────────────
#define DEDUP_CACHE_SIZE   64       // Ring buffer of recent message hashes
#define DEDUP_TTL_MS       (60 * 1000)  // Ignore duplicates within 60 s
