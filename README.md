# Meshtastic-Bitchat Bridge

An ESP32 firmware that bridges [Meshtastic](https://meshtastic.org/) LoRa mesh networks and [Bitchat](https://github.com/nicegram/nicegram-bitchat) BLE mesh networks, allowing users on either network to communicate with each other in real time.

## How It Works

```
┌──────────────┐         ┌──────────────────┐         ┌──────────────┐
│  Meshtastic  │◄──TCP──►│   ESP32 Bridge    │◄──BLE──►│   Bitchat    │
│  LoRa Mesh   │  :4403  │   (this firmware) │         │   BLE Mesh   │
└──────────────┘         └──────────────────┘         └──────────────┘
```

The bridge autodiscovers and connects to a Meshtastic node via mDNS (`meshtastic.local`) over TCP, and participates in the Bitchat BLE mesh as a native peer. Messages are forwarded bidirectionally with deduplication, identity mapping, and proper attribution.

### Key Features

- **Bidirectional message bridging** between Meshtastic and Bitchat networks
- **Virtual identities**: Each Meshtastic user appears as a distinct Bitchat peer with their own cryptographic identity, scaling dynamically based on available memory with graceful eviction
- **Name resolution**: Learns user names from Meshtastic NodeInfo and Bitchat announce packets; persists across reboots via NVS
- **Noise XX encryption**: Full Noise protocol handshake support for encrypted Bitchat sessions
- **Ed25519 signatures**: All outgoing packets are signed (including virtual identity packets)
- **BLE mesh gossip**: Relays packets between Bitchat peers with TTL-based flooding and deduplication
- **Packet fragmentation**: Automatic fragmentation/reassembly for messages exceeding BLE MTU
- **Dual-role BLE**: Acts as both BLE central and peripheral simultaneously

## Use Cases

**Off-grid events and gatherings** — At a music festival, campout, or field day, some people have Meshtastic radios for long-range comms while others only have phones running Bitchat. Drop a bridge on a picnic table and both groups can talk to each other without anyone installing new apps.

**Emergency and disaster response** — After a natural disaster knocks out cell towers, relief teams using Meshtastic LoRa radios can coordinate with nearby civilians using Bitchat on their phones over BLE. The bridge lets a single ESP32 connect both networks at a command post or shelter.

**Building or campus mesh** — In a large building, warehouse, or campus, Meshtastic nodes on rooftops provide long-range coverage while Bitchat handles room-to-room BLE communication. The bridge ties the two layers together so a message sent from a phone in one room reaches a radio operator across the property.

**Community mesh networks** — A neighborhood or small town runs Meshtastic for resilient off-grid communication. Visitors or newcomers who don't own a radio can join the conversation immediately using Bitchat on their phone, with the bridge making them visible to the LoRa side.

**Outdoor recreation** — A hiking group splits up on a trail. Some carry Meshtastic handhelds for long-range check-ins; others prefer the Bitchat app on their phone. A bridge in a backpack keeps both halves of the group in contact, even when BLE range and LoRa range only overlap at the bridge.

**Protocol development and testing** — Developers working on either the Meshtastic or Bitchat protocol can use the bridge to test interoperability, inspect cross-network message flow, and prototype new features without needing users on both networks simultaneously.

## Supported Hardware

| Board | Chip | Free heap (typical) | Virtual identities | Notes |
|-------|------|--------------------|--------------------|-------|
| ESP32-S3-DevKitC-1 | ESP32-S3 | ~250KB | 32 (hard cap) | Recommended. Heap never a constraint for identities. |
| ESP32-S3 + PSRAM | ESP32-S3 N8R8 | ~250KB internal + PSRAM | 32 (hard cap) | Most headroom. PSRAM available for future features. |
| ESP32-C6-DevKitC-1 | ESP32-C6 | ~160KB | 25-32 | Fully functional. May shed a few under heavy BLE load. |

### Memory budget

Each virtual identity costs **240 bytes** of heap (192-byte keypair + 48 bytes metadata). At the hard cap of 32 identities, that's only **7.5KB** — the identities themselves are never the bottleneck.

The real heap consumers are the BLE and WiFi stacks (~80KB), fragment reassembly buffers (~8.5KB for 4 slots), and per-peer Noise sessions (~400 bytes each). After all subsystems initialize, typical free heap is:

| Platform | Total SRAM | Free after init | Available above 40KB reserve | Identities before pressure |
|----------|-----------|----------------|------------------------------|---------------------------|
| ESP32-S3 | 512KB | ~250KB | ~210KB | 32 (capped long before pressure) |
| ESP32-S3 + PSRAM | 512KB + 8MB | ~250KB+ | ~210KB+ | 32 (capped) |
| ESP32-C6 | 512KB | ~160KB | ~120KB | 32 (capped, ~112KB still free) |

On all platforms, the 32-identity hard cap is reached well before memory pressure. The dynamic scaling and LRU eviction exist as a safety net for unexpected heap spikes (e.g. many simultaneous BLE connections, large fragmented messages, or future features that increase per-identity cost).

## Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or IDE extension)
- A Meshtastic node with **TCP API enabled** on port 4403 (Settings > Network > TCP)
- WiFi network accessible by both the ESP32 and the Meshtastic node

## Quick Start

### 1. Clone and configure

```bash
git clone https://github.com/SteveCohen/Bitchat-Meshtastic.git
cd Bitchat-Meshtastic
```

Edit `src/config.h` with your WiFi credentials:

```cpp
#define WIFI_SSID          "your-ssid"
#define WIFI_PASSWORD      "your-password"
```

That's it for most setups. The bridge autodiscovers your Meshtastic node via mDNS (`meshtastic.local`). If autodiscovery doesn't work, set the IP explicitly:

```cpp
#define MESHTASTIC_HOST    "192.168.1.100"   // IP address or mDNS hostname
```

Other optional settings:

```cpp
#define MESHTASTIC_PORT    4403              // TCP API port (default 4403)
#define BRIDGE_NAME        "BitBridge"       // Name shown in Bitchat
#define MESHTASTIC_CHANNEL 0                 // Meshtastic channel index
```

### 2. Build and flash

```bash
# ESP32-S3 (most common)
pio run -e esp32s3 -t upload

# ESP32-S3 with PSRAM
pio run -e esp32s3-psram -t upload

# ESP32-C6
pio run -e esp32c6 -t upload
```

### 3. Monitor

```bash
pio device monitor
```

You should see:

```
=================================
  Meshtastic-Bitchat Bridge
  BitBridge
=================================
Connecting to WiFi 'your-ssid'...
WiFi connected, IP: 192.168.1.50
mDNS responder started as BitBridge.local
NTP sync OK (epoch: 1712345678000)
[MeshTCP] Connecting to meshtastic.local:4403
[MeshTCP] Resolving mDNS hostname: meshtastic.local
[MeshTCP] Resolved meshtastic.local → 192.168.1.100
[MeshTCP] Got my_node_num: 12345678
[MeshTCP] Learned node 12345678: Alice (ALCE)
[MeshTCP] Config complete, my_node=12345678, known nodes=5
[Bridge] Bridge started
[BLE] Initialized, advertising as BitBridge
```

## Architecture

```
src/
├── main.cpp                    # Entry point, WiFi + NTP setup
├── config.h                    # All configuration constants
├── meshtastic/
│   ├── meshtastic_interface.h  # Abstract Meshtastic interface
│   ├── meshtastic_tcp.h/.cpp   # TCP client + protobuf parsing + node directory
│   ├── meshtastic_http.h       # HTTP API interface (alternative)
│   └── mesh_proto.h            # Hand-rolled protobuf encoder/decoder
├── bitchat/
│   ├── bitchat_interface.h     # Abstract Bitchat interface
│   ├── bitchat_ble.h/.cpp      # NimBLE implementation (central + peripheral)
│   ├── bitchat_identity.h      # Keypair generation, NVS persistence, derivation
│   ├── noise_state.h           # Noise_XX symmetric state (mbedtls)
│   ├── noise_handshake.h/.cpp  # Noise XX handshake state machine
│   └── ed25519.h               # Compact Ed25519 (tweetnacl-style, mbedtls SHA-512)
├── bridge/
│   ├── bridge_manager.h/.cpp   # Main bridge logic, dedup, message routing
│   ├── message.h               # BridgeMessage struct
│   ├── identity_mapper.h/.cpp  # Bidirectional name cache with NVS persistence
│   └── virtual_identity.h      # Virtual identity registry (HKDF-derived keypairs)
└── utils/
    └── time_util.h             # NTP time + epoch helpers
```

### Message Flow: Meshtastic to Bitchat

1. TCP client receives a `FromRadio` protobuf containing a text message
2. `MeshtasticTCP` resolves the sender's name from its node directory
3. `BridgeManager` deduplicates and looks up/creates a virtual identity for the sender
4. The message is sent over BLE as a signed `PKT_MESSAGE` using the virtual identity's keypair
5. Bitchat peers see the message as coming from a distinct peer named after the Meshtastic user

### Message Flow: Bitchat to Meshtastic

1. BLE callback receives a packet (plaintext `PKT_MESSAGE` or encrypted `PKT_NOISE_ENCRYPTED`)
2. `BitchatBLE` resolves the sender's nickname from the peer session
3. `BridgeManager` formats the message with a `[B]` prefix and sender name
4. The message is sent via TCP as a `ToRadio` protobuf text message

## Configuration Reference

All constants are in `src/config.h`:

| Constant | Default | Description |
|----------|---------|-------------|
| `MESHTASTIC_HOST` | `"meshtastic.local"` | Meshtastic node address (mDNS or IP) |
| `MESHTASTIC_PORT` | `4403` | Meshtastic TCP API port |
| `BRIDGE_NAME` | `"BitBridge"` | Name shown to Bitchat peers |
| `MESHTASTIC_CHANNEL` | `0` | Meshtastic channel to bridge |
| `VIRT_ID_INITIAL_SLOTS` | `4` | Pre-allocated virtual identity slots |
| `VIRT_ID_MAX_SLOTS` | `32` | Absolute upper limit on virtual identities |
| `VIRT_ID_HEAP_RESERVE_BYTES` | `40KB` | Minimum free heap before refusing new identities |
| `VIRT_ID_HEAP_CRITICAL_BYTES` | `25KB` | Below this, actively evict idle identities |
| `VIRT_ID_TIMEOUT_MS` | `10 min` | Expire idle virtual identities |
| `MAX_IDENTITY_ENTRIES` | `16` | Cached names per side (NVS-persisted) |
| `BITCHAT_MAX_CONNECTIONS` | `4` | Max simultaneous BLE connections |
| `BITCHAT_ANNOUNCE_INTERVAL_MS` | `60s` | Re-announce interval |
| `DEDUP_CACHE_SIZE` | `64` | Deduplication ring buffer size |
| `DEDUP_TTL_MS` | `60s` | Deduplication window |
| `MESH_HEARTBEAT_MS` | `5 min` | TCP keepalive interval |

## Protocol Details

### Meshtastic Side

Communicates via the Meshtastic TCP API (raw protobuf, no nanopb dependency). The firmware includes a hand-rolled protobuf encoder/decoder (`mesh_proto.h`) that handles:

- `ToRadio` / `FromRadio` framing (0x94 0xC3 + 2-byte length)
- Config handshake (`want_config_id` / `config_complete_id`)
- `MyNodeInfo` parsing (own node ID)
- `NodeInfo` parsing (peer names from `User.long_name` / `short_name`)
- `MeshPacket` / `Data` encoding/decoding for text messages
- Heartbeat packets

### Bitchat Side

Implements the Bitchat BLE protocol as a native peer using NimBLE:

- **BLE Service**: UUID `F47B5E2D-...` with a single read/write/notify characteristic
- **Packet types**: Announce (0x01), Message (0x02), Leave (0x03), Noise Handshake (0x10), Encrypted (0x11), Fragment (0x20)
- **TLV payload encoding** for all structured data
- **Noise_XX_25519_ChaChaPoly_SHA256** for encrypted private messaging
- **Ed25519 signatures** on all outgoing packets
- **TTL-based gossip** with relay deduplication

### Virtual Identities

When a Meshtastic user sends a message, the bridge derives a deterministic Bitchat identity for them:

```
HKDF-SHA256(
    IKM  = bridge_master_private_key,
    salt = "bitchat-virtual-id",
    info = node_id || purpose_byte
) → Curve25519 keypair + Ed25519 keypair + SHA-256 fingerprint
```

This means:
- The same Meshtastic user always gets the same Bitchat identity
- Virtual identities survive bridge reboots (deterministic, not stored)
- Each virtual identity announces, signs packets, and appears as a real Bitchat peer
- Currently uses plaintext messages (Option A); encrypted DMs per virtual identity is a future option

#### Dynamic scaling and memory management

The number of virtual identities scales automatically based on available heap memory rather than using a fixed slot count. The bridge starts with 4 slots and grows up to 32 as needed, as long as free heap stays above 40KB.

When memory runs low (below 25KB free), the bridge sheds identities by evicting the least-recently-used ones first. Both networks are notified:

- **Bitchat peers** see a farewell message from the departing identity: *"Alice has gone idle and left the chat. New messages from them will appear under BitBridge."*
- **Meshtastic users** see: *"Bridge released identity for Alice (memory pressure). Messages will still be bridged under BitBridge."*

Messages continue to flow through the bridge identity with `[M]` prefix attribution — no messages are lost, they just appear under the shared bridge name until the user becomes active again and a new virtual identity is allocated.

## Dependencies

- **[NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)** ^1.4.3 - BLE stack
- **ESP-IDF mbedtls** (bundled) - Curve25519, SHA-256, HKDF, ChaCha20-Poly1305
- No external protobuf library required

## License

See [LICENSE](LICENSE) for details.
