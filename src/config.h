#pragma once

// ── WiFi ──────────────────────────────────────────────
// Mode: "STA" = connect to existing network (default)
//       "AP"  = host a network (for battery/remote operation)
//       "AUTO"= try STA first, fall back to AP if connection fails
#define WIFI_MODE          "AUTO"

// Station mode (STA): connect to this network
#define WIFI_SSID          "your-ssid"
#define WIFI_PASSWORD      "your-password"

// Access point mode (AP): create this network
// The Meshtastic node connects to this network as a WiFi client.
#define WIFI_AP_SSID       "BitBridge"
#define WIFI_AP_PASSWORD   "bitbridge32"       // min 8 chars; "" for open network
#define WIFI_AP_CHANNEL    1
#define WIFI_AP_MAX_CLIENTS 4
#define WIFI_AP_IP         "192.168.4.1"       // Bridge IP in AP mode
#define WIFI_AP_GATEWAY    "192.168.4.1"
#define WIFI_AP_SUBNET     "255.255.255.0"

// ── Meshtastic node (TCP interface) ───────────────────
// Set MESHTASTIC_HOST to an IP address or mDNS hostname.
// Default "meshtastic.local" uses mDNS autodiscovery — no config needed
// if the Meshtastic node advertises itself via mDNS (most do by default).
#define MESHTASTIC_HOST    "meshtastic.local"
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
#define BITCHAT_HEADER_V1_LEN 14     // version(1)+type(1)+TTL(1)+timestamp(8)+flags(1)+payload_len(2)
#define BITCHAT_HEADER_V2_LEN 16     // version(1)+type(1)+TTL(1)+timestamp(8)+flags(1)+payload_len(4)
#define BITCHAT_HEADER_LEN    14     // default (v1) for outgoing packets
#define BITCHAT_SENDER_ID_LEN 8     // 8-byte peer ID (truncated SHA-256 of Noise pubkey)
// Bitchat BLE uses a custom service with a SINGLE characteristic for both directions
// (write to send, notify to receive)
#define BITCHAT_SERVICE_UUID        "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C"
#define BITCHAT_MSG_CHAR_UUID       "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D"

// ── Bitchat packet types (matches iOS permissionlesstech/bitchat) ──
#define BITCHAT_PKT_ANNOUNCE        0x01  // Identity/presence broadcast
#define BITCHAT_PKT_MESSAGE         0x02  // Public/channel text message
#define BITCHAT_PKT_LEAVE           0x03  // Peer departure
#define BITCHAT_PKT_NOISE_HANDSHAKE 0x10  // Noise XX handshake (raw bytes)
#define BITCHAT_PKT_NOISE_ENCRYPTED 0x11  // Encrypted payload (NoisePayload)
#define BITCHAT_PKT_FRAGMENT        0x20  // Message fragment
#define BITCHAT_PKT_REQUEST_SYNC    0x21  // Sync request

// ── Bitchat packet flags ──────────────────────────────
#define BITCHAT_FLAG_HAS_RECIPIENT  0x01
#define BITCHAT_FLAG_HAS_SIGNATURE  0x02
#define BITCHAT_FLAG_IS_COMPRESSED  0x04
#define BITCHAT_FLAG_IS_RELAY       0x08
#define BITCHAT_FLAG_IS_PRIVATE     0x10

// ── Bitchat TLV types for ANNOUNCE packets (0x01) ─────
#define BITCHAT_TLV_NICKNAME        0x01  // UTF-8 display name
#define BITCHAT_TLV_NOISE_PUBKEY    0x02  // Curve25519 static public key (32 bytes)
#define BITCHAT_TLV_SIGNING_PUBKEY  0x03  // Ed25519 signing public key (32 bytes)
#define BITCHAT_TLV_NEIGHBORS       0x04  // Direct neighbor peer IDs (N * 8 bytes)

// ── Bitchat TLV types for MESSAGE packets (0x02) ──────
#define BITCHAT_TLV_TEXT            0x05  // UTF-8 text content
#define BITCHAT_TLV_CHANNEL         0x07  // Channel identifier
#define BITCHAT_TLV_GEOHASH         0x08  // Location geohash

// ── PrivateMessagePacket inner TLV types ──────────────
#define BITCHAT_TLV_MESSAGE_ID      0x00  // 16-byte random message ID
#define BITCHAT_TLV_CONTENT         0x01  // UTF-8 message content

// ── NoisePayload types (inside decrypted 0x11 packets) ─
#define NOISE_PAYLOAD_PRIVATE_MSG   0x01  // Private message
#define NOISE_PAYLOAD_READ_RECEIPT  0x02  // Read receipt
#define NOISE_PAYLOAD_DELIVERED     0x03  // Delivery confirmation
#define NOISE_PAYLOAD_VERIFY_CHAL   0x10  // Verification challenge
#define NOISE_PAYLOAD_VERIFY_RESP   0x11  // Verification response

// ── Bitchat BLE parameters ───────────────────────────
#define BITCHAT_MAX_CONNECTIONS     4
#define BITCHAT_HANDSHAKE_TIMEOUT_MS (15 * 1000)  // Free peers stuck in handshake after 15s
#define BITCHAT_ANNOUNCE_INTERVAL_MS (60 * 1000)  // Re-announce every 60s
#define BITCHAT_SCAN_RSSI_MIN       (-70)
#define BITCHAT_BLE_MTU             512
#define BITCHAT_MAX_TEXT_LEN        100
#define BITCHAT_MSG_CACHE_SIZE      128
#define BITCHAT_MSG_CACHE_TTL_MS    (30 * 1000)

// ── Fragmentation ─────────────────────────────────────
// Fragment header inside PKT_FRAGMENT payload:
//   msg_id(4, random) + frag_idx(1) + total_frags(1) + chunk_data(N)
#define BITCHAT_FRAG_HEADER_LEN  6
#define BITCHAT_MAX_FRAGMENTS    8   // Max fragments per message
#define BITCHAT_FRAG_REASSEMBLY  4   // Concurrent reassembly slots
#define BITCHAT_FRAG_TIMEOUT_MS  (10 * 1000)  // 10s reassembly timeout

// ── Virtual identities (Phase 2b) ─────────────────────
// Identities scale dynamically based on available heap memory.
// Each identity uses ~230 bytes of heap.
#define VIRT_ID_INITIAL_SLOTS       4     // Pre-allocated slots at boot
#define VIRT_ID_MAX_SLOTS           32    // Absolute upper limit
#define VIRT_ID_HEAP_RESERVE_BYTES  (40 * 1024)  // Keep at least 40KB free heap
#define VIRT_ID_HEAP_CRITICAL_BYTES (25 * 1024)  // Below this, start evicting
#define VIRT_ID_TIMEOUT_MS          (10 * 60 * 1000)  // Expire after 10 min inactive
#define VIRT_ID_MEMORY_CHECK_MS     (10 * 1000)       // Check heap pressure every 10s
#define VIRTUAL_ANNOUNCE_STAGGER_MS 200   // Delay between virtual identity announces

// ── Identity mapping ──────────────────────────────────
#define MAX_IDENTITY_ENTRIES        16    // Max cached names per side (mesh + BLE)
#define IDENTITY_NVS_NAMESPACE      "idmap"
#define IDENTITY_SAVE_DEBOUNCE_MS   (30 * 1000)  // Min interval between NVS writes

// ── Deduplication ─────────────────────────────────────
#define DEDUP_CACHE_SIZE   64       // Ring buffer of recent message hashes
#define DEDUP_TTL_MS       (60 * 1000)  // Ignore duplicates within 60 s
