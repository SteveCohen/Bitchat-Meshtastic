# Meshtastic-Bitchat Bridge — Architecture Plan

## Overview

A bridge running on **ESP32-S3** or **ESP32-C6** that relays messages between a
**Meshtastic** LoRa mesh network and a **Bitchat** BLE mesh network. Users on
either network see messages from the other side rebroadcast into their own mesh.

```
┌──────────────┐       BLE        ┌─────────────────┐       TCP/4403       ┌──────────────────┐
│  Bitchat App │ ◄──────────────► │  ESP32-S3/C6    │ ◄──────────────────► │  Meshtastic Node │
│  (Phone)     │   bitchat mesh   │   BRIDGE        │   WiFi / Ethernet    │  (LoRa Radio)    │
└──────────────┘                  └─────────────────┘                      └──────────────────┘
```

### Supported Hardware

| Chip | Board | Environment | Notes |
|------|-------|-------------|-------|
| ESP32-S3 | DevKitC-1 N8 | `esp32s3` | Dual-core 240 MHz, no PSRAM |
| ESP32-S3 | DevKitC-1 N8R8/N16R8 | `esp32s3-psram` | With PSRAM |
| ESP32-C6 | DevKitC-1 | `esp32c6` | Single-core RISC-V 160 MHz, WiFi 6, BLE 5 |

Build for your board: `pio run -e esp32c6` (or `esp32s3`)

## Design Principles

1. **Modular transports** — Meshtastic side uses an abstract interface; TCP is
   implemented first, HTTP can be swapped in later.
2. **Bitchat-compatible** — The bridge participates in the BLE mesh as a
   standard bitchat peer so existing apps work unmodified.
3. **Identity placeholders** — Stub layer for future per-user identity mapping
   (bitchat Noise keys ↔ Meshtastic node IDs).
4. **Simple first** — Phase 1 is plain-text rebroadcast in both directions
   with a single bridge identity. Phase 2 adds per-user identity spoofing.

---

## Phase 1 — Plain Rebroadcast (current target)

### Meshtastic Side (TCP Interface)

| Detail | Value |
|--------|-------|
| Transport | TCP socket to Meshtastic node |
| Port | 4403 (`DEFAULT_TCP_PORT`) |
| Framing | `0x94 0xC3` + 2-byte BE length + protobuf payload |
| Max payload | 512 bytes |
| Protobuf | `ToRadio` / `FromRadio` (nanopb) |
| Text messages | `MeshPacket.decoded.portnum = TEXT_MESSAGE_APP (1)` |
| Broadcast dest | `0xFFFFFFFF` |

**Connection lifecycle:**
1. TCP connect to `<meshtastic_host>:4403`
2. Send 32 bytes of `0xC3` (wake/reset)
3. Send `ToRadio { want_config_id: <nonce> }`
4. Drain `FromRadio` until `config_complete_id == nonce`
5. Enter message loop: read `FromRadio` packets, send `ToRadio` packets
6. Heartbeat every 300 s

**Sending text:**
```
ToRadio {
  packet: MeshPacket {
    to: 0xFFFFFFFF,
    channel: 0,
    id: <generated>,
    hop_limit: 3,
    decoded: Data {
      portnum: TEXT_MESSAGE_APP (1),
      payload: <UTF-8 bytes>
    }
  }
}
```

**Receiving text:**
- Parse `FromRadio.packet.decoded` where `portnum == 1`
- `payload` is UTF-8 text, `from` is sender node number

### Bitchat Side (BLE Interface)

| Detail | Value |
|--------|-------|
| Transport | BLE GATT (custom service, single characteristic) |
| BLE Service UUID | `F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C` |
| BLE Msg Char UUID | `A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D` |
| Char properties | WRITE + WRITE_NR + NOTIFY (single char both directions) |
| BLE MTU | 512 bytes |
| Encryption | Noise_XX_25519_ChaChaPoly_SHA256 |
| Identity | Curve25519 (Noise) + Ed25519 (signatures) |
| Peer ID | First 8 bytes of SHA-256(Noise static public key) |
| Packet header | 14 bytes: version(1), type(1), TTL(1), timestamp(8), flags(1), payload_len(2) |
| Variable fields | sender_id(8B), [recipient_id(8B)], TLV payload, [signature(64B)] |
| Payload encoding | TLV: type(1B) + length(1B) + value(NB) |
| TLV types | NICKNAME=0x01, TEXT=0x05, CHANNEL=0x07, PUBKEY=0x09, etc. |
| Max hops | 7 (TTL) |
| Max connections | 4 simultaneous BLE peers |
| Scan RSSI threshold | -70 dBm |
| Routing | Gossip flooding with hash-based dedup (128 entries, 30s TTL) |
| Padding | PKCS#7-style to 256/512/1024/2048 byte blocks |

**Phase 1 simplification:** The bridge acts as a single bitchat identity.
All Meshtastic messages appear to come from "Bridge" on the bitchat side.
All bitchat messages appear as `[BLE] <text>` on the Meshtastic side.

### Bridge Manager

The bridge manager:
1. Initializes both interfaces
2. Registers callbacks for incoming messages on each side
3. On Meshtastic message → format and send to Bitchat
4. On Bitchat message → format and send to Meshtastic
5. Applies deduplication (don't re-bridge a message we originated)
6. Prefixes messages with origin indicator: `[M]` for Meshtastic, `[B]` for Bitchat

### Message Flow

```
Meshtastic user sends "Hello"
  → FromRadio received on TCP
  → Bridge extracts text + sender node ID
  → Bridge creates bitchat packet: "[M] !abcd1234: Hello"
  → Packet broadcast over BLE mesh
  → Bitchat users see the message

Bitchat user sends "Hi there"
  → BLE packet received
  → Bridge extracts text + sender fingerprint
  → Bridge creates ToRadio: "[B] a1b2...c3d4: Hi there"
  → Sent over TCP to Meshtastic node
  → Meshtastic users see the message
```

---

## Phase 2 — Per-User Identity Mapping (future)

### Goal
Each bitchat user appears as a distinct identity on Meshtastic and vice versa.

### Approach
- Maintain a mapping table: `bitchat_fingerprint ↔ virtual_meshtastic_node_id`
- On the Meshtastic side this may require multiple logical nodes or message
  prefixing (Meshtastic doesn't support impersonating arbitrary node IDs via
  the client API)
- On the bitchat side, the bridge could maintain multiple Noise sessions,
  one per Meshtastic user, each with a derived key pair
- The `IdentityMapper` class provides the placeholder for this

### Identity Mapper Interface
```cpp
class IdentityMapper {
    // Map a Meshtastic node ID to a bitchat display identity
    BitchatIdentity meshTobitchat(uint32_t meshtastic_node_id);

    // Map a bitchat fingerprint to a Meshtastic display identity
    MeshtasticIdentity bitchatToMesh(const uint8_t fingerprint[32]);

    // Store/load persistent mappings
    void save();
    void load();
};
```

---

## Project Structure

```
├── PLAN.md                          # This file
├── platformio.ini                   # PlatformIO build config (ESP32-S3/C6)
├── proto/
│   └── meshtastic/
│       ├── mesh.proto               # Core Meshtastic protobufs (subset)
│       └── portnums.proto           # Port number enums
├── src/
│   ├── main.cpp                     # Entry point: init WiFi, start bridge
│   ├── config.h                     # WiFi credentials, Meshtastic host, etc.
│   ├── bridge/
│   │   ├── bridge_manager.h         # Core bridge: wire interfaces + routing
│   │   ├── bridge_manager.cpp
│   │   ├── message.h                # Unified BridgeMessage struct
│   │   └── identity_mapper.h        # Phase 2 placeholder
│   ├── meshtastic/
│   │   ├── meshtastic_interface.h   # Abstract interface
│   │   ├── meshtastic_tcp.h         # TCP implementation
│   │   ├── meshtastic_tcp.cpp
│   │   ├── meshtastic_http.h        # HTTP placeholder
│   │   └── mesh_proto.h             # Minimal hand-rolled protobuf helpers
│   └── bitchat/
│       ├── bitchat_interface.h      # Abstract interface
│       ├── bitchat_ble.h            # BLE implementation
│       ├── bitchat_ble.cpp
│       └── bitchat_identity.h       # Bitchat identity/key stubs
```

---

## Technology Choices

| Choice | Rationale |
|--------|-----------|
| **PlatformIO + ESP-IDF** | Full ESP32-S3/C6 support, BLE + WiFi stacks |
| **Hand-rolled protobuf** | Only need ToRadio/FromRadio text messages; avoids nanopb build complexity initially |
| **C++ (Arduino-like)** | Familiar, good ESP32 ecosystem, PlatformIO default |
| **NimBLE** | Lightweight BLE stack included with ESP-IDF, supports GATT server + central role |

---

## Configuration

All runtime config via `config.h` defines (compile-time for Phase 1):

```cpp
#define WIFI_SSID          "your-ssid"
#define WIFI_PASSWORD      "your-password"
#define MESHTASTIC_HOST    "192.168.1.100"
#define MESHTASTIC_PORT    4403
#define BRIDGE_NAME        "BitBridge"
#define MESHTASTIC_CHANNEL 0           // Primary channel
#define MSG_PREFIX_MESH    "[M]"
#define MSG_PREFIX_BLE     "[B]"
```

---

## Open Questions

1. ~~**Bitchat BLE service UUIDs**~~ — RESOLVED: Service UUID `F47B5E2D...`,
   Msg Char UUID `A1B2C3D4...` (from bitchat-esp32 source).
2. **Noise handshake on bridge** — Phase 1 may skip encryption and only
   relay plaintext broadcast messages. Phase 2 requires a full Noise
   implementation.
3. **Message size limits** — Meshtastic payload max is 233 bytes; bitchat
   packets are also small. Long messages may need truncation.
4. **Deduplication** — Need to track recent message hashes to avoid echo loops.
5. **Meshtastic identity spoofing** — The TCP API doesn't support sending as
   arbitrary node IDs. Phase 2 may need MQTT or direct radio integration.
