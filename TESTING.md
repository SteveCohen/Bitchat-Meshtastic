# Testing Plan: Meshtastic-Bitchat Bridge with Geohash Scoping

This document walks through testing from initial flash to full geohash scoping validation. Follow the phases in order — each builds on the previous one.

---

## Equipment needed

| Item | Quantity | Notes |
|------|----------|-------|
| ESP32-S3-DevKitC-1 | 1 | The bridge. N8 (no PSRAM) is fine. |
| Meshtastic radio node | 1+ | Any supported hardware (T-Beam, Heltec, RAK, etc.) with TCP API enabled. A second node is useful for verifying LoRa delivery. |
| Phone with Bitchat app | 1+ | iOS (from permissionlesstech/bitchat). Two phones is ideal for testing BLE relay. |
| Phone/tablet with Meshtastic app | 1 | For sending/receiving on the LoRa side. Can be the same device as above if it runs both apps. |
| USB-C cable | 1 | For flashing and serial monitor. |
| WiFi network | 1 | Shared between ESP32 and Meshtastic node. Or use AP mode (Phase 1c). |
| Computer with PlatformIO | 1 | For building and flashing. VS Code + PlatformIO extension is easiest. |

**Optional but helpful:**
- Second ESP32-S3 (for two-bridge geohash testing in Phase 5)
- nRF Connect app (Android/iOS) for raw BLE packet inspection
- Second Meshtastic radio (to verify LoRa propagation independently of the bridge)

---

## Phase 1: Initial Flash and Basic Connectivity

### 1a. Build environment setup

```bash
# Install PlatformIO CLI (if not already installed)
pip install platformio

# Clone the repo
git clone https://github.com/SteveCohen/Bitchat-Meshtastic.git
cd Bitchat-Meshtastic

# Checkout the geohash branch
git checkout claude/bitchat-meshtastic-geohash-A0fSH
```

### 1b. Configure and flash (STA mode — home WiFi)

Edit `src/config.h`:

```cpp
#define WIFI_MODE          "STA"
#define WIFI_SSID          "YourWiFiName"
#define WIFI_PASSWORD      "YourWiFiPassword"

// Leave geohash DISABLED for Phase 1
#define GEOHASH_SCOPE_ENABLED       false
#define BRIDGE_GEOHASH              ""
#define BRIDGE_GEOHASH_PRECISION    4
```

Build and flash:

```bash
pio run -e esp32s3 -t upload
```

Open serial monitor:

```bash
pio device monitor
```

**Expected output (pass criteria):**

- [ ] `Connecting to WiFi 'YourWiFiName'...` followed by `WiFi connected, IP: 192.168.x.x`
- [ ] `mDNS responder started as BitBridge.local`
- [ ] `NTP sync OK` (or NTP failure warning — non-fatal)
- [ ] `[BLE] Initialized, advertising as BitBridge`
- [ ] No crash/reboot loops. Free heap reported > 200KB.

**If Meshtastic node is on the same network:**

- [ ] `[MeshTCP] Connecting to meshtastic.local:4403`
- [ ] `[MeshTCP] Config complete, my_node=XXXXXXXX, known nodes=N`
- [ ] `[Bridge] Bridge started`

**Troubleshooting:**
- If mDNS fails, set `MESHTASTIC_HOST` to the node's IP address explicitly.
- If WiFi won't connect, double-check SSID/password, try `"AUTO"` mode.
- If serial output looks garbled, confirm `monitor_speed = 115200` matches.

### 1c. AP mode test (optional but recommended for field use)

Change config:

```cpp
#define WIFI_MODE          "AP"
#define WIFI_AP_SSID       "BitBridge"
#define WIFI_AP_PASSWORD   "bitbridge32"
```

Reflash. Configure your Meshtastic node to join the "BitBridge" WiFi network (Meshtastic app > Settings > Network > WiFi).

**Pass criteria:**

- [ ] Serial shows `AP mode: SSID=BitBridge`
- [ ] Meshtastic node connects to the AP (check Meshtastic app network status)
- [ ] `[MeshTCP] Config complete` appears in serial log
- [ ] NTP warning appears (expected — no internet in AP mode)

---

## Phase 2: Basic Bridging (Geohash OFF)

This phase tests that the bridge works correctly in its default configuration before enabling geohash scoping. All messages should flow freely in both directions.

### 2a. Meshtastic → Bitchat

**Setup:** Meshtastic app on one device, Bitchat app on a phone near the ESP32.

**Steps:**

1. Open Bitchat on a phone. Wait for the bridge to appear as a peer (up to 60s for announce cycle).
2. On the Meshtastic app, send a text message on channel 0: `"Hello from Meshtastic"`

**Pass criteria:**

- [ ] Serial log shows: `[MeshTCP] Text from XXXXXXXX: "Hello from Meshtastic"`
- [ ] Serial log shows: `[Bridge] Mesh→BLE (virtual <name>): Hello from Meshtastic` (or `Mesh→BLE:` for fallback)
- [ ] Bitchat app displays the message. If virtual identity is working, it appears from a peer named after the Meshtastic user. Otherwise it appears from "BitBridge" with `[M]` prefix.

### 2b. Bitchat → Meshtastic

**Steps:**

1. On the Bitchat app, send a text message: `"Hello from Bitchat"`
2. Check the Meshtastic app for the message.

**Pass criteria:**

- [ ] Serial log shows: `[BLE] Message from <name>: "Hello from Bitchat"`
- [ ] Serial log shows: `[Bridge] BLE→Mesh: [B] <name>: Hello from Bitchat`
- [ ] Meshtastic app displays: `[B] <name>: Hello from Bitchat`

### 2c. Deduplication

**Steps:**

1. Send the same message from Meshtastic twice quickly (within 60s).
2. Send a message from Bitchat and watch the serial log for echo handling.

**Pass criteria:**

- [ ] Serial log shows `Dedup: skipping` for the repeated message
- [ ] No echo loops (a message doesn't bounce back and forth endlessly)

### 2d. Virtual identity

**Steps:**

1. Send messages from two different Meshtastic nodes (or send from one, reboot, send from another).
2. Check how they appear in Bitchat's peer list.

**Pass criteria:**

- [ ] Each Meshtastic user appears as a distinct Bitchat peer with their own name
- [ ] Serial log shows `Mesh→BLE (virtual <name>)` for each
- [ ] Bitchat peer list shows the virtual identities (may take up to 60s for announce)

### 2e. Bidirectional stress test

**Steps:**

1. Send 10 messages in rapid succession from Meshtastic.
2. Send 10 messages in rapid succession from Bitchat.
3. Interleave: alternate sending from each side.

**Pass criteria:**

- [ ] All messages delivered (some delay is acceptable on LoRa side due to duty cycle)
- [ ] No crashes. Monitor free heap in serial output — should stay above 150KB.
- [ ] No duplicate messages on either side.

---

## Phase 3: Enable Geohash Scoping

Now enable the geohash feature. First, look up your location's geohash at [geohash.org](http://geohash.org/) — you'll need at least 6 characters.

### 3a. Configure and reflash

Edit `src/config.h`:

```cpp
#define GEOHASH_SCOPE_ENABLED       true
#define BRIDGE_GEOHASH              "9q8yyk"    // ← Replace with YOUR geohash
#define BRIDGE_GEOHASH_PRECISION    4
```

Reflash:

```bash
pio run -e esp32s3 -t upload
```

### 3b. Verify backward compatibility (messages with no geohash)

**This is the most important test.** Existing Bitchat clients don't set geohashes yet. We must confirm they still work.

**Steps:**

1. Send a message from Bitchat (which won't include a geohash TLV).
2. Send a message from Meshtastic.

**Pass criteria:**

- [ ] Bitchat → Meshtastic: message forwards normally. Serial log shows `BLE→Mesh:` with NO geohash log line (no geohash = always bridged, silently).
- [ ] Meshtastic → Bitchat: message arrives in Bitchat. Serial log shows `Mesh→BLE`.
- [ ] **Behavior is identical to Phase 2.** Enabling geohash scoping with no geohash-aware clients must not break anything.

### 3c. Verify Meshtastic → BLE geohash tagging

**Steps:**

1. Send a message from Meshtastic.
2. Watch serial log carefully.

**What to look for:** The bridge should tag the outgoing BLE packet with the region geohash. With the config above (`BRIDGE_GEOHASH="9q8yyk"`, precision 4), outgoing packets should carry geohash `"9q8y"`.

**Pass criteria:**

- [ ] Serial log shows `Mesh→BLE` as before (the geohash tagging is silent in current logging)
- [ ] No errors or crashes

**Advanced verification (requires nRF Connect or BLE sniffer):**

- [ ] Inspect the raw BLE packet. After the `TLV_TEXT (0x05)` entry, there should be a `TLV_GEOHASH (0x08)` entry containing 4 bytes: the ASCII chars of `"9q8y"`.

---

## Phase 4: Geohash Filtering (BLE → Meshtastic)

This is the core geohash scoping test. Since current Bitchat clients don't set geohashes, you'll need to either:

**Option A:** Modify the Bitchat app to include a geohash TLV in outgoing messages (ideal but requires app development).

**Option B:** Use a second ESP32 running a test harness that sends BLE packets with specific geohash TLVs (requires custom firmware).

**Option C:** Temporarily modify the bridge itself to inject a test geohash into incoming BLE messages for testing purposes.

For Option C (quickest path), you can add a temporary test hook in `bridge_manager.cpp` inside `_on_bitchat_message()`. This is described below.

### 4a. Test: Block-level message stays local

**Goal:** A BLE message with a high-precision geohash (e.g., 7 chars) should NOT be forwarded to Meshtastic.

**Temporary test patch** (add after dedup check in `_on_bitchat_message`, before the `_should_bridge_to_meshtastic` call):

```cpp
// TEMPORARY TEST: Simulate geohash on incoming BLE messages
// Remove this block after testing!
BridgeMessage test_msg = msg;
strncpy(test_msg.geohash, "9q8yyk8", sizeof(test_msg.geohash) - 1);  // 7 chars = block-level
```

Then use `test_msg` instead of `msg` for the `_should_bridge_to_meshtastic` call.

**Pass criteria:**

- [ ] Serial log shows: `[Bridge] Geohash '9q8yyk8' (precision 7 > 4): keeping local (BLE only)`
- [ ] Message does NOT appear on Meshtastic
- [ ] Message still delivered locally on BLE mesh (the bridge just doesn't forward it)

### 4b. Test: Region-level message gets bridged

**Same approach, but with a short geohash:**

```cpp
strncpy(test_msg.geohash, "9q8y", sizeof(test_msg.geohash) - 1);  // 4 chars = region-level
```

**Pass criteria:**

- [ ] Serial log shows: `[Bridge] Geohash '9q8y' (precision 4 <= 4): forwarding to Meshtastic`
- [ ] Message appears on Meshtastic app

### 4c. Test: Boundary precision values

Test with geohashes at exactly the threshold and one above:

| Test geohash | Precision | Expected behavior |
|---|---|---|
| `"9q8y"` | 4 (= threshold) | **Bridged** to Meshtastic |
| `"9q8yy"` | 5 (> threshold) | **Kept local** on BLE |
| `"9q8"` | 3 (< threshold) | **Bridged** to Meshtastic |
| `""` | 0 (none) | **Bridged** (backward compat) |

**Pass criteria for each:**

- [ ] `"9q8y"` (4): `forwarding to Meshtastic` — appears on Meshtastic
- [ ] `"9q8yy"` (5): `keeping local (BLE only)` — does NOT appear on Meshtastic
- [ ] `"9q8"` (3): `forwarding to Meshtastic` — appears on Meshtastic
- [ ] `""` (none): forwards silently — appears on Meshtastic

### 4d. Test: Different precision thresholds

Change `BRIDGE_GEOHASH_PRECISION` to `5`, reflash, and repeat 4a-4c with updated expectations:

```cpp
#define BRIDGE_GEOHASH_PRECISION    5
```

Now a 5-char geohash should be bridged, and 6+ chars should stay local.

**Pass criteria:**

- [ ] `"9q8yy"` (5): now **bridged** (was kept local at threshold 4)
- [ ] `"9q8yyk"` (6): **kept local**
- [ ] `"9q8y"` (4): still **bridged**

**Remember to remove the temporary test patch and reflash when done.**

---

## Phase 5: Multi-Bridge / Geofencing Scenarios

These tests require additional hardware or can be validated conceptually if equipment is limited.

### 5a. Two bridges, different geohashes (requires 2x ESP32)

**Setup:**

```
Bridge A                              Bridge B
BRIDGE_GEOHASH = "dp3wt7"            BRIDGE_GEOHASH = "dp3wt9"
BRIDGE_GEOHASH_PRECISION = 5         BRIDGE_GEOHASH_PRECISION = 5
         │                                     │
    BLE mesh A                            BLE mesh B
    (phones near A)                    (phones near B)
         │                                     │
         └──────── Shared Meshtastic ──────────┘
                   LoRa network
```

**Tests:**

- [ ] Block-level message (7 chars) on mesh A stays on mesh A only. Bridge B never sees it (it never hits Meshtastic).
- [ ] Region-level message (5 chars) on mesh A is bridged to Meshtastic. Bridge B receives it and injects into mesh B (with its own region geohash `"dp3wt"`).
- [ ] Messages from Meshtastic arrive on both BLE meshes, each tagged with the respective bridge's region geohash.

### 5b. Geofence mismatch (future feature — conceptual test)

**Note:** The current implementation does NOT verify geohash prefix matching. This test documents what *should* happen once that feature is added.

**Scenario:** A message scoped to `"dp3w"` (Chicago area) arrives at a bridge with `BRIDGE_GEOHASH="9q8yyk"` (San Francisco).

**Current behavior:** The bridge forwards it anyway (no prefix check).

**Expected future behavior:** The bridge should check that the message's geohash prefix matches its own and drop mismatches. This prevents messages intended for one region from leaking into another through an unrelated bridge.

- [ ] Document this as a known limitation for now.

### 5c. Mixed client test

**Setup:** Mix of geohash-aware and legacy (no geohash) Bitchat clients on the same BLE mesh.

**Tests:**

- [ ] Legacy client sends message (no geohash): bridged to Meshtastic. ✓ backward compat.
- [ ] Geohash-aware client sends block-level message: stays local. Legacy clients on the same BLE mesh still see it (it's still a normal BLE packet, just not forwarded to Meshtastic).
- [ ] Geohash-aware client sends region-level message: bridged to Meshtastic. Both legacy and geohash-aware clients see it locally AND it appears on Meshtastic.

---

## Phase 6: Edge Cases and Robustness

### 6a. Geohash enabled but empty BRIDGE_GEOHASH

```cpp
#define GEOHASH_SCOPE_ENABLED       true
#define BRIDGE_GEOHASH              ""          // Empty!
#define BRIDGE_GEOHASH_PRECISION    4
```

**Pass criteria:**

- [ ] All messages bridged unconditionally (empty geohash = scoping effectively disabled)
- [ ] No crashes. The `_bridge_region_geohash` returns `nullptr`, `_should_bridge_to_meshtastic` returns `true`.

### 6b. BRIDGE_GEOHASH shorter than BRIDGE_GEOHASH_PRECISION

```cpp
#define BRIDGE_GEOHASH              "9q"        // Only 2 chars
#define BRIDGE_GEOHASH_PRECISION    4
```

**Pass criteria:**

- [ ] Outgoing geohash on Meshtastic→BLE messages is `"9q"` (truncated to source length, not padded)
- [ ] No crash or buffer overrun.

### 6c. Maximum length geohash in incoming message

A BLE message with an 11-character geohash (maximum the `geohash[12]` field can hold).

**Pass criteria:**

- [ ] Geohash extracted correctly (11 chars + null terminator)
- [ ] Precision = 11, which is > 4, so it stays local
- [ ] No buffer overflow

### 6d. Bridge reboot with geohash enabled

**Steps:**

1. Enable geohash scoping, flash, and verify it works.
2. Power cycle the ESP32.
3. Send messages from both sides.

**Pass criteria:**

- [ ] Bridge reconnects to Meshtastic after reboot
- [ ] Geohash scoping still works (compile-time config, no NVS dependency)
- [ ] Virtual identities regenerate deterministically (same Meshtastic user = same Bitchat identity)

### 6e. High message volume with geohash filtering

**Steps:**

1. With geohash enabled, send 20 rapid block-level (local) messages from BLE.
2. Send 20 rapid region-level messages from BLE.
3. Send 20 messages from Meshtastic simultaneously.

**Pass criteria:**

- [ ] Block-level messages all filtered (none reach Meshtastic)
- [ ] Region-level messages all forwarded
- [ ] No heap leak — free heap before and after should be within ~1KB
- [ ] No crash, no watchdog reset

---

## Phase 7: Cleanup and Production Readiness

### 7a. Remove any test patches

- [ ] Remove temporary geohash injection code from Phase 4 if added
- [ ] Verify clean build: `pio run -e esp32s3`

### 7b. Final configuration review

Decide on your production settings:

```cpp
// Do you want geohash scoping in production?
#define GEOHASH_SCOPE_ENABLED       true        // or false to ship without it

// Your deployment location
#define BRIDGE_GEOHASH              "XXXXXX"    // Your 6+ char geohash

// Your network's range
#define BRIDGE_GEOHASH_PRECISION    4           // 4 for most LoRa setups
```

### 7c. Full regression

Run through these one final time with production config:

- [ ] Phase 2a: Meshtastic → Bitchat works
- [ ] Phase 2b: Bitchat → Meshtastic works
- [ ] Phase 2c: Dedup works
- [ ] Phase 2d: Virtual identities work
- [ ] Phase 3b: Backward compat (no-geohash messages bridge)
- [ ] Phase 4a-4c: Geohash filtering works (if enabled)

### 7d. Flash production firmware

```bash
pio run -e esp32s3 -t upload
```

Disconnect serial monitor. Deploy.

---

## Quick Reference: What to Re-Test After Config Changes

| Change | Re-test phases |
|--------|----------------|
| Changed `WIFI_MODE` or WiFi credentials | 1b or 1c |
| Changed `MESHTASTIC_HOST` or `MESHTASTIC_PORT` | 1b, 2a |
| Changed `GEOHASH_SCOPE_ENABLED` (toggle on/off) | 3b, 4a-4c |
| Changed `BRIDGE_GEOHASH` | 3c, 4a-4c |
| Changed `BRIDGE_GEOHASH_PRECISION` | 4a-4d |
| Changed `VIRT_ID_MAX_SLOTS` or memory thresholds | 2d, 6e |
| Changed `BITCHAT_MAX_CONNECTIONS` | 2a-2e |
| Upgraded Bitchat app (now sends geohash) | 4a-4c (without test patch!), 5c |
| Added second bridge | 5a |
| Any firmware update | 7c (full regression) |

---

## Known Limitations

1. **No geohash prefix matching yet.** The bridge doesn't verify that a message's geohash belongs to its geographic area. A message scoped to Chicago could be bridged by a San Francisco bridge. See Phase 5b.

2. **Bitchat app doesn't send geohashes yet.** Until the Bitchat iOS app is updated to include `TLV_GEOHASH (0x08)` in outgoing messages, all BLE messages have no geohash and are bridged unconditionally. The feature is ready on the bridge side.

3. **Bitchat app doesn't display geohash scope.** Meshtastic messages enter BLE with a region-level geohash TLV, but the Bitchat app currently ignores it. No visual distinction between local and regional messages yet.

4. **Compile-time configuration only.** Changing geohash or precision requires reflashing. Runtime configuration (NVS or BLE characteristic) is a future feature.

5. **No GPS integration.** The bridge's geohash is static. A moving bridge (e.g., in a vehicle) won't update its geohash automatically.
