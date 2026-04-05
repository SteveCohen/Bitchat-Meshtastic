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
- **Geohash scoping**: Optional geographic message scoping — high-precision geohashes stay on the local BLE mesh ("block talk"), low-precision geohashes get bridged to Meshtastic ("town square")

## Geohash Scoping

### The problem

Bitchat BLE and Meshtastic LoRa operate at fundamentally different scales. A BLE mesh covers a city block (~30-50m per hop). A Meshtastic LoRa mesh covers a city or a mountain range (5-20km+). Without any scoping, *every* BLE message gets forwarded to the entire LoRa network, and every LoRa message floods into the local BLE mesh. There's no way for people on a BLE block to have a local conversation that doesn't broadcast to the region, and no way for a Meshtastic user to tell whether a message was meant for the neighborhood or the whole network.

### The idea: geohash precision = message scope

A [geohash](https://en.wikipedia.org/wiki/Geohash) is a string that encodes a geographic area. The key property is that **shorter geohashes cover larger areas**:

| Geohash | Precision | Area covered | Conceptual scope |
|---------|-----------|--------------|------------------|
| `9q8y` | 4 chars | ~40km x 20km | City / LoRa range |
| `9q8yy` | 5 chars | ~5km x 5km | Neighborhood |
| `9q8yyk` | 6 chars | ~1.2km x 0.6km | A few blocks |
| `9q8yyk8` | 7 chars | ~150m x 150m | One block / BLE range |
| `9q8yyk8v` | 8 chars | ~38m x 19m | A single building |

This maps naturally to the BLE/Meshtastic range difference. The bridge uses a **precision threshold** to decide what gets forwarded:

```
                        BRIDGE_GEOHASH_PRECISION = 4
                                    │
                   bridged to       │     stays on local
                   Meshtastic       │     BLE mesh only
                   (town square)    │     (block talk)
                                    │
   ◄────────────────────────────────┼────────────────────────────►
   precision 1    2    3    4       │    5    6    7    8
   (continent)                      │              (building)
```

- **Messages with geohash precision <= threshold** (4 chars or fewer) are forwarded to Meshtastic. These are "town square" messages meant for the wider region.
- **Messages with geohash precision > threshold** (5+ chars) stay on the local BLE mesh. These are "block talk" — neighborhood conversations that don't need to go regional.
- **Messages with no geohash** are always forwarded. This preserves backward compatibility with existing Bitchat clients that don't set geohashes.

### Why this is spiritually correct

Bitchat is local-first. Your BLE mesh is your block, your building, your campsite. It's the people physically near you. Meshtastic extends that to the town, the valley, the mountain range — but that wider reach should be intentional, not automatic.

Geohash scoping preserves this by making **local the default and regional the opt-in**. A Bitchat user on your block who tags their message with a fine-grained geohash (7-8 chars) is saying "this is for the neighborhood." The bridge respects that and keeps it local. A user who tags with a coarse geohash (4 chars) is saying "this is for the town square" — and the bridge forwards it to Meshtastic.

No internet needed. No TOR needed. Meshtastic *is* the wide-area transport, scoped by geographic intention. The bridge is the boundary between your local community and the regional network, and geohash precision is the knob that controls where that boundary sits.

### Configuration

Three settings in `src/config.h` control geohash scoping:

```cpp
#define GEOHASH_SCOPE_ENABLED       false       // Master toggle
#define BRIDGE_GEOHASH              ""          // Bridge location, e.g. "9q8yyk"
#define BRIDGE_GEOHASH_PRECISION    4           // Messages with precision > this stay BLE-only
```

**To enable geohash scoping**, set all three:

```cpp
#define GEOHASH_SCOPE_ENABLED       true
#define BRIDGE_GEOHASH              "9q8yyk"    // Your location's geohash
#define BRIDGE_GEOHASH_PRECISION    4           // 4 = city-scale bridging
```

You can look up your geohash at [geohash.org](http://geohash.org/) or any geohash tool — enter your coordinates and use at least 6 characters of precision.

**When disabled** (the default), all messages are bridged unconditionally, exactly as before. You can deploy this firmware without changing any behavior.

### How to choose your precision threshold

The threshold controls the boundary between "local" and "regional." Here are some practical guidelines:

| `BRIDGE_GEOHASH_PRECISION` | What gets bridged to Meshtastic | What stays BLE-only | Good for |
|---|---|---|---|
| **3** | Messages scoped to ~150km+ areas | Anything city-scale or smaller | Very large LoRa networks spanning multiple cities |
| **4** (default) | Messages scoped to ~40km areas | Neighborhood and block conversations | Most setups. Matches typical Meshtastic range. |
| **5** | Messages scoped to ~5km areas | Block-level conversations | Dense urban areas where you want neighborhood-scale bridging |
| **6** | Messages scoped to ~1km areas | Building-level conversations | Campus or building mesh where blocks are distinct communities |

**Rule of thumb**: set the threshold to match your Meshtastic network's practical range. If your LoRa nodes can reliably reach 20km, precision 4 (40km x 20km) is appropriate. If you're in a hilly area where LoRa only reaches 5km, precision 5 (5km x 5km) might be better.

### Examples

#### Example 1: Festival with block-level privacy

A music festival spans several fields. Each field has a cluster of Bitchat users. One bridge serves the whole site, connected to a Meshtastic network that covers the festival grounds and parking areas.

```cpp
#define GEOHASH_SCOPE_ENABLED       true
#define BRIDGE_GEOHASH              "9q8yyk"
#define BRIDGE_GEOHASH_PRECISION    4
```

- **Main Stage field**: Users chat with geohash `"9q8yyk8v"` (8 chars, building-level). Their banter about the current band stays on the local BLE mesh. People at other stages don't see it.
- **Lost and found announcement**: A user sends with geohash `"9q8y"` (4 chars, festival-wide). The bridge forwards this to Meshtastic. Everyone on the LoRa network sees it.
- **Legacy Bitchat user**: Someone with an older app that doesn't set geohashes sends "Has anyone seen my dog?" — no geohash means it gets bridged to Meshtastic by default. No one is silenced by the new feature.

#### Example 2: Neighborhood mesh with regional backbone

A neighborhood runs a few Meshtastic nodes on rooftops for resilient comms. Each block has people using Bitchat on their phones. A bridge on each block connects the two.

```
Block A (Bridge A, geohash "dp3wt7")     Block B (Bridge B, geohash "dp3wt9")
  ┌──────────────────────┐                  ┌──────────────────────┐
  │  BLE mesh: 8 phones  │                  │  BLE mesh: 5 phones  │
  │  Local chat stays    ├──── Meshtastic ──┤  Local chat stays    │
  │  on Block A          │   LoRa backbone  │  on Block B          │
  └──────────────────────┘                  └──────────────────────┘
```

```cpp
// Both bridges use the same threshold, different geohashes
#define GEOHASH_SCOPE_ENABLED       true
#define BRIDGE_GEOHASH              "dp3wt7"   // (or "dp3wt9" for Bridge B)
#define BRIDGE_GEOHASH_PRECISION    5
```

- **Block A gossip** (geohash `"dp3wt7k"`, 7 chars): Stays on Block A's BLE mesh. Bridge A doesn't forward it. Block B never sees it.
- **Neighborhood alert** (geohash `"dp3wt"`, 5 chars): Both bridges forward to Meshtastic. Everyone on both blocks sees it.
- **City-wide emergency** (geohash `"dp3w"`, 4 chars): Forwarded by both bridges. Reaches the entire Meshtastic network.

#### Example 3: Backward-compatible deployment

You want to try the firmware but aren't ready to commit to geohash scoping. Just don't enable it:

```cpp
#define GEOHASH_SCOPE_ENABLED       false      // Default — everything bridged
#define BRIDGE_GEOHASH              ""
#define BRIDGE_GEOHASH_PRECISION    4
```

All messages flow both directions unconditionally, exactly as before. You can enable scoping later by changing one line and reflashing.

### What Meshtastic messages look like on BLE

When a Meshtastic message arrives at the bridge, it enters the BLE mesh tagged with the bridge's geohash truncated to the configured precision. For example, if `BRIDGE_GEOHASH` is `"9q8yyk"` and `BRIDGE_GEOHASH_PRECISION` is `4`, Meshtastic messages enter BLE with geohash `"9q8y"`.

This tells Bitchat clients that the message came from the wider regional network, not from the local block. Future Bitchat apps could use this to visually distinguish local vs. regional messages, or to filter by scope.

### Serial log output

When geohash scoping is enabled, the bridge logs its decisions:

```
[Bridge] Geohash '9q8yyk8' (precision 7 > 4): keeping local (BLE only)
[Bridge] Geohash '9q8y' (precision 4 <= 4): forwarding to Meshtastic
```

Messages with no geohash are forwarded silently (no extra log line) to avoid noise.

### Future directions

- **GPS-based geohash**: Replace the compile-time `BRIDGE_GEOHASH` with a runtime GPS reading, so mobile bridges (e.g., in a backpack) automatically update their location.
- **Geohash prefix matching**: Before bridging a region-scoped message, verify that its geohash prefix matches the bridge's. A message scoped to `"dp3w"` (Chicago) shouldn't be bridged by a San Francisco bridge with geohash `"9q8y"`.
- **Per-channel scoping**: Different Meshtastic channels could have different precision thresholds — e.g., an emergency channel that bridges everything regardless of precision.
- **Client-side scope display**: Bitchat apps could render local vs. regional messages differently based on the geohash TLV, or let users choose their sending scope with a slider.
- **Runtime configuration**: Store geohash and precision in NVS, configurable via BLE characteristic or serial command, so you don't need to reflash to change settings.

## Use Cases

**Off-grid events and gatherings** — At a music festival, campout, or field day, some people have Meshtastic radios for long-range comms while others only have phones running Bitchat. Drop a bridge on a picnic table (AP mode, battery-powered, no WiFi router needed) and both groups can talk to each other without anyone installing new apps.

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

Edit `src/config.h`. The only required settings are WiFi credentials:

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
#define WIFI_MODE          "AUTO"            // "STA", "AP", or "AUTO" (see below)
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

### AP Mode (battery / remote operation)

For field use without existing WiFi infrastructure, the bridge can host its own WiFi network. The Meshtastic node joins this network as a client, and the two devices communicate over their own private LAN — powered by batteries, no router needed.

```
                    ┌─── WiFi AP ───┐
┌──────────────┐    │ ┌───────────┐ │    ┌──────────────┐
│  Meshtastic  │◄───┼►│  ESP32    │◄┼─BLE──►│   Bitchat    │
│  LoRa Radio  │WiFi│ │  Bridge   │ │    │   Phones     │
└──────────────┘    │ │ (AP mode) │ │    └──────────────┘
                    │ └───────────┘ │
     battery ──────►│  192.168.4.x  │
                    └───────────────┘
```

**Setup:**

1. Set `WIFI_MODE` in `config.h`:

```cpp
#define WIFI_MODE        "AP"              // Host a network
#define WIFI_AP_SSID     "BitBridge"       // Network name
#define WIFI_AP_PASSWORD "bitbridge32"     // Min 8 chars, or "" for open
```

2. Configure your Meshtastic node to connect to the `BitBridge` WiFi network (Settings > Network > WiFi > SSID/Password). Enable TCP API on port 4403.

3. Flash and power both devices. The bridge creates the network, the Meshtastic node joins and gets a DHCP address (192.168.4.x), and the bridge finds it via mDNS or you can set `MESHTASTIC_HOST` to a fixed IP.

**WiFi modes:**

| Mode | `WIFI_MODE` | Behavior |
|------|-------------|----------|
| Station | `"STA"` | Connect to an existing WiFi network (home/office). Default. |
| Access Point | `"AP"` | Create a WiFi network. No internet, no NTP — timestamps use `millis()`. |
| Auto | `"AUTO"` | Try STA first; if it fails (no network in range), automatically fall back to AP. Best for devices that move between field and home. |

**Notes for AP mode:**
- No internet access — NTP time sync is unavailable, so bitchat packet timestamps use `millis()` (time since boot) rather than wall-clock time. This is fine for message ordering but means timestamps won't match between reboots.
- The Meshtastic node must be configured to connect to the bridge's WiFi network and have TCP API enabled.
- The bridge runs a DHCP server (built into ESP32 `softAP`) on the 192.168.4.0/24 subnet.
- mDNS still works on the AP network, so `meshtastic.local` resolution works if the Meshtastic node supports it.

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
| `WIFI_MODE` | `"AUTO"` | `"STA"`, `"AP"`, or `"AUTO"` |
| `WIFI_AP_SSID` | `"BitBridge"` | Network name in AP mode |
| `WIFI_AP_PASSWORD` | `"bitbridge32"` | AP password (min 8 chars, `""` for open) |
| `MESHTASTIC_HOST` | `"meshtastic.local"` | Meshtastic node address (mDNS or IP) |
| `MESHTASTIC_PORT` | `4403` | Meshtastic TCP API port |
| `BRIDGE_NAME` | `"BitBridge"` | Name shown to Bitchat peers |
| `MESHTASTIC_CHANNEL` | `0` | Meshtastic channel to bridge |
| `GEOHASH_SCOPE_ENABLED` | `false` | Enable geohash-based message scoping |
| `BRIDGE_GEOHASH` | `""` | Bridge location geohash (e.g. `"9q8yyk"`) |
| `BRIDGE_GEOHASH_PRECISION` | `4` | Messages with precision > this stay BLE-only |
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

#### Why 32 identities? Why not unlimited?

The cap isn't about memory — 32 identities use only 7.5KB. It exists because of real constraints in the radio and BLE layers:

**BLE announce flooding.** Every virtual identity re-announces itself every 60 seconds to stay visible to Bitchat peers. Each announce is a ~180-byte signed packet sent to every connected BLE peer. At 32 identities with 4 BLE connections, that's 128 announce packets per minute. BLE 4.2 on the ESP32 tops out around 200-300 small packets/second under ideal conditions, but real-world throughput with connection event scheduling is much lower. Beyond 32 identities, announce traffic alone would start crowding out actual messages.

**Bitchat peer list usability.** Bitchat apps display every announced peer in a contact list. A bridge that injects 100 virtual identities would flood the peer list on every nearby phone, burying real local BLE users under a wall of Meshtastic names. 32 is already a lot — it covers the realistic number of active participants in a Meshtastic channel.

**Meshtastic channel capacity.** Meshtastic LoRa channels on default long-range settings (Long Fast, SF11) support roughly 10-20 messages per minute before duty cycle limits kick in. Even on Short Fast with higher throughput, sustained traffic from more than ~30 unique senders is unusual. The cap matches the practical ceiling of how many distinct users you'd actually see active in a Meshtastic channel window.

**Loop latency.** The bridge processes announces, messages, memory checks, and reconnection logic in a single-threaded `loop()`. More identities means more work per iteration — keypair derivation on first creation takes ~5ms (HKDF + Curve25519 scalar multiply), and each announce cycle with 200ms stagger delays adds 200ms * N to the loop. At 32 identities that's 6.4 seconds of announce stagger per cycle, which is acceptable within a 60-second interval but would become dominant at higher counts.

**What happens when you hit the cap.** If a 33rd Meshtastic user sends a message while 32 identities are active, the bridge evicts the least-recently-used identity (with farewell notifications) and allocates a new one for the incoming user. The evicted user's messages still bridge through the shared bridge name with `[M]` prefix. If they send again later, they get a fresh virtual identity — with the same keypair as before (deterministic), so Bitchat peers recognize them as the same person.

You can raise `VIRT_ID_MAX_SLOTS` in `config.h` if your use case genuinely has more than 32 concurrent Meshtastic users, but the BLE announce overhead becomes the practical limit well before memory does.

#### BLE connections vs. virtual identities

It's important to understand that BLE connections and virtual identities are independent axes, and the BLE connection limit is the tighter constraint on how many people can actually use the bridge:

```
                          ┌─────────────────────────────────────────────────┐
Meshtastic                │             ESP32 Bridge                        │             Bitchat
LoRa Mesh                 │                                                 │             BLE Mesh
                          │  ┌─────────────────────────────┐                │
  Alice ─────┐            │  │  Virtual Identities (heap)  │                │
  Bob ───────┤            │  │                             │  BLE conn 1 ──────── Phone A
  Carol ─────┤◄── TCP ───►│  │  "Alice"  "Bob"  "Carol"   │  BLE conn 2 ──────── Phone B
  Dave ──────┤            │  │  "Dave"   "Eve"  ...       │  BLE conn 3 ──────── Phone C
  Eve ───────┘            │  │  (up to 32, shared over     │  BLE conn 4 ──────── Phone D
                          │  │   all BLE connections)      │                │
                          │  └─────────────────────────────┘                │
                          └─────────────────────────────────────────────────┘
```

**Virtual identities** are a logical construct — they're just keypairs and metadata in memory. They don't consume BLE connections. All 32 virtual identities share the same physical BLE connections, sending packets with different `sender_id` fields over the same GATT characteristic.

**BLE connections** are the physical radio links to nearby phones. The ESP32 NimBLE stack supports a limited number of concurrent connections. The bridge is configured for **4 simultaneous connections** (`BITCHAT_MAX_CONNECTIONS`), acting as both central and peripheral:

| Limit | ESP32-S3 | ESP32-C6 | Why |
|-------|----------|----------|-----|
| NimBLE max connections | 9 | 9 | Firmware-configurable, but each connection consumes ~1.5KB RAM + radio scheduling slots |
| Configured limit | 4 | 4 | Default. Higher values reduce per-connection throughput due to radio time-sharing |
| Practical sweet spot | 3-5 | 3-4 | Balances reach vs. per-connection bandwidth |

**What this means in practice:**

- **Only 4 Bitchat phones can connect directly to the bridge** — but this does NOT mean only 4 people can receive messages. Bitchat is a mesh network. The connected phones relay packets to their own BLE peers, who relay further, and so on. The bridge sends once; the mesh delivers.

- **All 32 virtual identities are visible to all connected phones.** Each phone sees up to 32 Meshtastic users appear as distinct Bitchat peers. The identities multiplex over the physical connections — no per-identity connection is needed.

- **Throughput scales with connections, not identities.** Each BLE connection operates at roughly 10-20KB/s with a 512-byte MTU and typical 30-50ms connection intervals. With 4 connections, total BLE throughput is ~40-80KB/s. Announces for 32 identities (180 bytes each, every 60s, to 4 peers) consume ~380 bytes/second — under 1% of available bandwidth.

- **More connections = less bandwidth per connection.** The ESP32 has a single BLE radio. NimBLE schedules connection events across all active connections in a round-robin fashion. Going from 4 to 8 connections roughly halves the throughput available to each connection. For a bridge that's mostly forwarding short text messages, 4 connections is a good balance.

- **You can raise `BITCHAT_MAX_CONNECTIONS`** in `config.h` up to 9, but beyond 5-6 connections you'll notice increased latency on individual messages as the radio has less time per connection. The Meshtastic side (TCP over WiFi) has no such limit — it can handle traffic from the entire LoRa mesh.

#### BLE mesh gossip: how 4 connections reach 30 people

The bridge doesn't need a direct connection to every Bitchat phone. The Bitchat protocol uses TTL-based gossip flooding — every peer relays packets it receives to all its other connections, decrementing the TTL until it hits zero. The bridge just needs to get the packet into the mesh; the mesh does the rest.

Here's what a realistic scenario looks like. Say there are 12 Bitchat users at a campsite, spread across BLE range (~30-50m outdoors). The bridge can only connect to 4 of them directly, but the message reaches everyone:

```
                                                    ┌─── Phone H
                                    ┌─── Phone E ───┤
                 ┌─── Phone A ──────┤               └─── Phone I
                 │                  └─── Phone F
  ESP32 Bridge ──┤
    (sends 4     ├─── Phone B ────────── Phone G ──────── Phone J
     packets)    │
                 ├─── Phone C
                 │
                 └─── Phone D ──────┬─── Phone K
                                    └─── Phone L
```

1. Bridge sends a message to Phones A, B, C, D (4 packets out).
2. Phone A relays to Phones E, F. Phone D relays to Phones K, L. Phone B relays to G.
3. Phone E relays to H, I. Phone G relays to J.
4. All 12 people have the message. The bridge sent 4 packets — the mesh handled the other 8.

**Key details:**

- **The bridge sends each packet exactly once per direct connection.** It does not round-robin or retry. One write per connected peer per message — the relay logic is on the receiving phones, not the bridge.

- **Relay deduplication prevents loops.** Every node (including the bridge) keeps a hash cache of recently-seen packets. If Phone E receives the same packet from both Phone A and Phone B, it relays it only once.

- **TTL limits propagation depth.** Packets start with TTL=7 (`BITCHAT_MAX_HOPS`). Each relay decrements it. This prevents packets from bouncing around forever in dense meshes, and caps the propagation radius at 7 hops — more than enough for any realistic BLE gathering.

- **Announces propagate the same way.** When the bridge announces a virtual identity, that announce relays through the mesh. Phones that aren't directly connected to the bridge still see the virtual identity appear in their peer list after a hop or two.

- **The bridge also relays inbound.** When Phone K sends a message, Phone D receives it and relays it to the bridge (and to Phone L). The bridge doesn't need a direct connection to Phone K — it reaches the bridge through the mesh, then gets forwarded to Meshtastic.

**What this means for sizing:** The 4-connection limit is about direct radio links, not audience size. In practice, a dense group of 20-30 Bitchat phones within a few hops of each other will all receive bridged messages, even though only 4 are directly connected to the ESP32. The limiting factor for large groups is BLE range between phones (each hop needs peers within ~30-50m of each other) and the TTL depth (7 hops), not the bridge's connection count.

## Dependencies

- **[NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)** ^1.4.3 - BLE stack
- **ESP-IDF mbedtls** (bundled) - Curve25519, SHA-256, HKDF, ChaCha20-Poly1305
- No external protobuf library required

## License

See [LICENSE](LICENSE) for details.
