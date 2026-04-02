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
#define BITCHAT_HEADER_LEN    13
// TODO: Discover actual BLE service/characteristic UUIDs from bitchat app
#define BITCHAT_SERVICE_UUID        "00000001-0000-1000-8000-00805f9b34fb"  // placeholder
#define BITCHAT_CHAR_TX_UUID        "00000002-0000-1000-8000-00805f9b34fb"  // placeholder
#define BITCHAT_CHAR_RX_UUID        "00000003-0000-1000-8000-00805f9b34fb"  // placeholder

// ── Deduplication ─────────────────────────────────────
#define DEDUP_CACHE_SIZE   64       // Ring buffer of recent message hashes
#define DEDUP_TTL_MS       (60 * 1000)  // Ignore duplicates within 60 s
