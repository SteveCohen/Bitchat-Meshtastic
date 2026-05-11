# Bitchat-Meshtastic Bridge — Improvement TODO

A code-review-driven punch list of issues, correctness gaps, security
weaknesses, and robustness problems found in the current implementation,
along with directive guidance on how to fix each one. Items are grouped by
severity and ordered roughly by priority. File references use
`path:line_number` so you can jump straight to the code.

---

## CRITICAL — Correctness bugs that produce wrong behaviour or crashes

### 1. The bridge's own messages can echo back through BLE gossip
**Where:** `src/bridge/bridge_manager.cpp:160-165`, `:176-181`,
`src/bitchat/bitchat_ble.cpp:912-914`

When `_on_meshtastic_message` forwards a message to BLE as a virtual
identity, it records a dedup hash via:

```cpp
BridgeMessage outgoing;
strncpy(outgoing.text, msg.text, sizeof(outgoing.text) - 1);
_record_hash(outgoing.hash());
```

`outgoing.sender` is default-initialised to all zeros, but
`BridgeMessage::hash()` (see `src/bridge/message.h:48-60`) mixes the
`meshtastic_node_id` and `bitchat_fingerprint[0..8]` into the hash. When the
message is gossiped back to us from another BLE peer, the incoming
`BridgeMessage` will have a real (non-zero) `bitchat_fingerprint` — so the
hashes will not match and the bridge will treat the echo as a new message.
Result: chat loops between BLE and Mesh whenever more than one BLE peer
relays.

**Fix directive:**
- Stop relying on the post-bridge dedup hash for outgoing-echo suppression.
  Instead, in `BitchatBLE::_process_incoming`, reject packets whose
  `sender_id` matches either our master fingerprint *or* any active virtual
  identity fingerprint (drop both before delivery and before relay).
- Maintain a small set (e.g. `std::vector<std::array<uint8_t,8>>`) of
  "our-identities" in `BitchatBLE`, updated when virtual identities are
  added/evicted; check it in `_process_incoming` right after computing
  `sender_id`.
- As a belt-and-braces measure, fix the dedup record in `bridge_manager` to
  use the *actual* outgoing sender id for the hash (populate
  `outgoing.sender.bitchat_fingerprint[0..8]` from `vi->keypair.fingerprint`
  before calling `outgoing.hash()`).

---

### 2. `_send_announce` does not record its packet in the relay dedup cache
**Where:** `src/bitchat/bitchat_ble.cpp:675-714` vs `:482`, `:536`, `:629`

`_send_text_as`, `announce_virtual`, and `_send_packet` all call
`_relay_record(_relay_hash(packet, pos))` before sending. `_send_announce`
(the bridge's own bridge-identity announce) does **not**. When a peer
relays our announce back to us, we will then re-relay it (because the hash
isn't in our dedup cache), creating an O(N²) announce storm in any topology
with multiple peers.

**Fix directive:** Add the same `_relay_record(_relay_hash(packet, pos))`
call to `_send_announce` immediately before `_pad_packet`. Apply the same
fix to `_send_handshake_packet`, `_send_encrypted`, and `_send_leave` —
even though those packet types are *not* currently relayed, this is
defence-in-depth against future changes.

---

### 3. Relay decrements TTL but signatures are over the original TTL byte
**Where:** `src/bitchat/bitchat_ble.cpp:1353-1377`, signature path
`:854-884`

`_relay_packet` copies the packet, does `relay_buf[2] = relay_buf[2] - 1`,
and forwards. The Ed25519 signature in the trailing 64 bytes covers the
*pre-decrement* TTL. Any downstream verifier that hashes the whole packet
(including TTL) when verifying — which is what our own code does
(`ed25519_verify(sig, pkt, signed_len, ...)`) — will reject the relayed
packet. Result: signature verification works on direct hops but fails on
gossip-relayed packets; the warning log `Packet signature INVALID` will
flood once peers start relaying.

**Fix directive:** Pick one of:
- **(Preferred)** Verify signatures over the packet with TTL byte
  normalised to a constant (e.g. zero). Update both `_process_incoming`
  signature-verify path and any sign path to either sign a TTL-zeroed copy
  or define TTL as part of the unauthenticated header. Document which
  bytes are signed.
- Or, drop relay support for signed packets and require Bitchat peers to
  do the relaying themselves (less interoperable).

Also verify what the reference iOS `permissionlesstech/bitchat`
implementation does and match it exactly; this is a wire-format contract.

---

### 4. Fragment reassembly stage size is smaller than what the sender produces
**Where:** Sender `src/bitchat/bitchat_ble.cpp:1159-1163`,
receiver `:1232`, `:1279`

Sender computes `max_chunk = BITCHAT_BLE_MTU - 14 - 8 - 6 - 64 - 64 = 356`
bytes per fragment. Receiver allocates stage slots of
`sizeof(FragReassembly::data) / BITCHAT_MAX_FRAGMENTS = 2048 / 8 = 256`
bytes per index. Any fragment 257..356 bytes long will fail the
`stage_offset + chunk_len <= sizeof(slot->data)` check on the last index
(or silently corrupt earlier indices' stage areas if the offset math
happens to allow it).

Additionally `RX_BUF_SIZE = BITCHAT_BLE_MTU = 512`, so the reassembled
packet is dropped at the ring-buffer enqueue if it exceeds 512 bytes
(`src/bitchat/bitchat_ble.cpp:1308`) — meaning fragmentation only ever
"works" for packets that didn't need fragmenting.

**Fix directive:**
- Make `FragReassembly::data` and the per-index stage area large enough
  for the worst-case fragment (e.g. `BITCHAT_MAX_FRAGMENTS * 384` bytes
  with a documented per-index stride matching the sender's `max_chunk`).
- Expand the RX ring buffer entry to accept reassembled packets (a
  separate, larger code path that bypasses `RX_BUF_SIZE`, or raise
  `RX_BUF_SIZE` to 4096).
- Add a `static_assert` tying `BITCHAT_MAX_FRAGMENTS`,
  `BITCHAT_BLE_MTU`, and the reassembly buffer sizes together so future
  config changes can't silently re-introduce this skew.

---

### 5. AAD-length mismatch when peer sends a v2 (16-byte header) encrypted packet
**Where:** Encrypt `src/bitchat/bitchat_ble.cpp:580-583`,
decrypt `:1092-1095`, version detection `:803-822`

`_send_encrypted` always uses `BITCHAT_HEADER_LEN` (14, v1) as the AAD
length when calling `noise_hs_encrypt`. The decrypt path passes the
*actual* header length (14 for v1, 16 for v2) from `_process_incoming`.
If a peer ever sends us a v2-framed encrypted packet, our decryption will
include the extra 2 header bytes as AAD; the sender's encryption did not
— authentication tag mismatch → drop.

**Fix directive:** Pick one consistent header version per session (v1 is
fine) and use it everywhere on both encrypt and decrypt, or read the
version off the packet on the encrypt side. Add a unit test that round-
trips encrypt/decrypt for both header layouts.

---

## HIGH — Security weaknesses

### 6. No verification that announce `sender_id` is `SHA256(noise_pub)[0..8]`
**Where:** `src/bitchat/bitchat_ble.cpp:919-976`

The protocol's identity binding is: peer_id is the first 8 bytes of
SHA-256 over the Noise static public key. When we receive an announce, we
extract `BITCHAT_TLV_NOISE_PUBKEY` from the TLV but never check that
`SHA256(noise_pubkey)[0..8] == sender_id`. A peer can claim any
fingerprint they like by sending a mismatched announce — fine for them as
a self-DoS, but it also means cached `peer->nickname`, `peer->sign_pubkey`,
and the eventual Noise handshake's `rs_pub` may all be associated with the
wrong peer ID.

**Fix directive:** In `_handle_announce`, compute
`noise_sha256(noise_pub, 32, computed_fp)` and reject the announce (drop
silently, log at debug) if `memcmp(computed_fp, sender_id, 8) != 0`.

---

### 7. Unsigned and unverifiable announces are accepted
**Where:** `src/bitchat/bitchat_ble.cpp:863-873`

```cpp
if (sign_pub && sign_pub_len == 32) {
    if (ed25519_verify(...) != 0) { ... drop ... }
}
// No signing pubkey in announce — can't verify, allow through
```

Falling through "allow through" defeats the purpose of authentication: a
forger can simply omit `BITCHAT_TLV_SIGNING_PUBKEY` from a malicious
announce and have it accepted unchallenged.

**Fix directive:** Drop announces that lack a signing pubkey or whose
signature is missing/invalid. Keep a config-flag override for
`STRICT_AUTH` for backwards compatibility with very old peers, defaulting
to strict.

---

### 8. Pre-announce signed packets are accepted unverified
**Where:** `src/bitchat/bitchat_ble.cpp:881-883`

```cpp
} else {
    Serial.printf("[%s] WARN: Signed packet but no known pubkey for peer, allowing\n", TAG);
}
```

A peer can send a signed `MESSAGE` packet *before* their `ANNOUNCE` and
we'll deliver it without verification, then later receive an announce with
*any* arbitrary signing pubkey. Combine with the bug above and a forger
can inject messages attributed to anyone.

**Fix directive:** Hold any signed non-announce packet until we have
verified the peer's announce. Either queue (one slot per peer, with a
short timeout) or drop entirely with a debug log.

---

### 9. Noise handshake doesn't bind `rs_pub` to the announced `noise_pubkey`
**Where:** `src/bitchat/noise_handshake.cpp:125-132` (msg2 path),
`:176-182` (msg3 path)

The XX pattern transmits the responder's/initiator's static keys (`rs_pub`)
inside the handshake, encrypted-and-hashed. We never cross-check that the
`rs_pub` learned during the handshake matches the `noise_pubkey` the peer
advertised in their `ANNOUNCE`. So a peer can announce one identity but
then complete a Noise session as a different one — undermining the whole
"this is a confidential session with the announced peer" property.

**Fix directive:** After the handshake completes (transition to
`NOISE_HS_TRANSPORT`), compare `peer->hs.rs_pub` with the
`noise_pubkey` stored from `_handle_announce`. If they differ, tear down
the session and free the peer slot. This requires storing the announced
`noise_pubkey` on `PeerSession` (currently only `sign_pubkey` is stored
— see `bitchat_ble.h:32`).

---

### 10. Padding is ambiguous when the data length already aligns
**Where:** `src/bitchat/bitchat_ble.cpp:81-101`

```cpp
int pad_len = target - data_len;
if (pad_len <= 0) return data_len;  // no padding
```

If `data_len` already equals `target` (256/512/1024/2048), no padding is
added. The receiver's `unpad_packet` then interprets the final byte as a
pad length and may strip a legitimate signature byte. PKCS#7 *requires* a
full block of padding when aligned to avoid exactly this ambiguity.

**Fix directive:**
- When `pad_len == 0`, append a full block (`pad_len = block_size`) of
  pad bytes equal to `block_size`. Document the new padded sizes.
- On unpad, refuse decoded `pv == 0`; require strict `1 <= pv <= block`
  and verify *all* trailing `pv` bytes equal `pv` before stripping.
- Add a round-trip test for both aligned and unaligned plaintexts.

---

## MEDIUM — Robustness and reliability

### 11. WiFi STA is set-and-forget — no reconnect on drop
**Where:** `src/main.cpp:20-41`

`wifi_connect_sta` is called once in `setup()`. If WiFi is lost later (AP
reboot, signal drop), nothing reconnects it. The Meshtastic side will
report `DISCONNECTED` forever; the device must be physically rebooted.

**Fix directive:** Register a WiFi event handler
(`WiFi.onEvent(...)`) for `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` that
issues `WiFi.reconnect()`. Track consecutive failures and apply
exponential backoff (`5s → 5min`) similar to the Meshtastic reconnect
loop in `BridgeManager::loop`. In AUTO mode, fall back to AP after, say,
5 minutes of STA failure.

---

### 12. Blocking delays in `setup()` (up to ~35 seconds)
**Where:** `src/main.cpp:25-30` (20 s STA), `:127-131` (5 s NTP),
`src/meshtastic/meshtastic_tcp.cpp:199` (5 s mDNS),
`src/meshtastic/meshtastic_tcp.cpp:225-231` (10 s config handshake)

`setup()` is allowed to take a long time, but the BLE side starts *only
after* WiFi finishes. A phone trying to connect during the first ~20s
won't see any BLE peer.

**Fix directive:**
- Re-order so `bitchat.begin()` runs first, then WiFi, then mDNS, then
  Meshtastic connect.
- Move the Meshtastic config handshake into the main loop (state machine)
  so it doesn't block; it already retries via `BridgeManager::_mesh_retry_*`.
- Reduce mDNS timeout to 2 s and retry from the main loop.

---

### 13. Stack usage: 2 KB stack buffers in many send paths
**Where:** `src/bitchat/bitchat_ble.cpp:420`, `:459`, `:558`, `:601`,
`:749`, `:1193`, `:1292`, `:1365`

Multiple `uint8_t packet[2048]`, `uint8_t relay_buf[2048]`,
`uint8_t assembled[2048]` allocations on the main task stack. ESP32 task
stacks default to ~8 KB; if `_process_incoming` recurses (relay → send →
relay) or runs concurrent to a `_handle_fragment` reassembly call, this
can blow the stack and crash the radio with no useful diagnostics.

**Fix directive:**
- Hoist these into a single re-usable member buffer
  (`uint8_t _tx_scratch[2048]` in `BitchatBLE`), accessed under the loop
  task. None of these paths are re-entrant from another task.
- Add a compile-time check on main task stack size in
  `sdkconfig.defaults` and/or bump the BLE task stack.
- Consider raising `RX_BUF_SIZE` and re-using one heap allocation rather
  than stack scratch.

---

### 14. Identity mapper capacity is smaller than virtual identity capacity
**Where:** `src/config.h:151` (`MAX_IDENTITY_ENTRIES = 16`) vs `:143`
(`VIRT_ID_MAX_SLOTS = 32`)

We can have up to 32 virtual identities live at once, but only 16 names
will be persisted across reboots. The other 16 names silently fail to
save (see `IdentityMapper::update_mesh_name`, `identity_mapper.cpp:43`).

**Fix directive:** Either:
- Raise `MAX_IDENTITY_ENTRIES` to ≥ `VIRT_ID_MAX_SLOTS` and confirm NVS
  blob size still fits.
- Or, switch to LRU eviction on the identity mapper too (mirror the
  `VirtualIdentityRegistry` policy) so identities and names age out
  together.

Also: on shutdown / power-down hook (e.g. ESP32 brownout detector), flush
the dirty NVS state. Today, debounced writes can lose 30 seconds of
updates if power drops mid-window.

---

### 15. No replay-attack protection on transport-mode Noise messages
**Where:** `src/bitchat/noise_state.h:238-283`

Once a Noise session is established, decryption rejects messages with the
wrong AEAD tag for the (k, n) tuple — but the nonce counter `cs->n` only
moves forward locally. If a peer disconnects mid-session and a later
attacker replays an old encrypted packet *before* we receive new ones,
the receive nonce will still match. ChaCha20-Poly1305 is malleable across
nonces only if the same (k, n) is reused, which our spec prevents — but
the bigger concern is that we don't enforce strict nonce monotonicity
when packets arrive *out of order* or when the peer's `cs->n` gets ahead.

**Fix directive:**
- Maintain a small replay window (e.g. 32 nonces, bitmask-based) on the
  receive side. Reject anything below the window's low-water mark.
- Tear down and re-handshake on consecutive auth failures.

---

### 16. No retransmit for lost handshake messages
**Where:** `src/bitchat/bitchat_ble.cpp:962-974` (initiator),
`:1027-1078` (state machine)

If msg2 is lost en route, the initiator sits in `NOISE_HS_AWAIT_MSG2`
until `BITCHAT_HANDSHAKE_TIMEOUT_MS` (15 s) and we free the peer slot.
Single packet loss = no session for 15 s and a full re-scan.

**Fix directive:** Add per-peer handshake retransmit at ~3 s intervals
(up to 3 retries) for the *last sent* handshake message. Keep a copy of
the most recent outbound message bytes on `PeerSession`.

---

### 17. Memory leaks: BLE callback objects never freed
**Where:** `src/bitchat/bitchat_ble.cpp:207` (server cb),
`:215` (char cb), `:322` (client cb)

`new BitchatBLEServerCallbacks(this)` etc. are passed by raw pointer to
NimBLE and never deleted. `BitchatBLE::end()` only calls
`NimBLEDevice::deinit(true)`. Re-initialising the BLE stack (e.g. after a
fatal handshake error → end() → begin()) leaks ~32 bytes each.

**Fix directive:** Store the callback objects as members (so they're
destroyed with the `BitchatBLE` instance) or in unique_ptr fields. Same
for `_g_ble_instance` global — fold into the class.

---

### 18. Verbose `Serial.printf` in every hot path
**Where:** Most modules

Every received packet, every announce, every handshake step emits one or
more Serial lines. At 115200 baud this is ~10 ms per line and can stall
the loop under load. There's no debug-level gate.

**Fix directive:**
- Introduce a `BRIDGE_LOG_LEVEL` macro in `config.h`
  (0=silent, 1=error, 2=warn, 3=info, 4=debug) and wrap printfs.
- Drop per-fragment, per-receipt, per-relay-dedup INFO logs to DEBUG.
- Optionally route logs to a ring buffer that the status report dumps
  on demand.

---

## LOW — Code quality and maintainability

### 19. The Meshtastic HTTP transport is a placeholder
**Where:** `src/meshtastic/meshtastic_http.h`

Five `TODO` markers, no implementation. If we don't intend to ship it,
delete the file — dead headers age into rot. If we do, finish it.

**Fix directive:** Implement against `MeshtasticInterface` using
`HTTPClient` and the existing `mesh_proto.h` encoders. Add an env
variable / config flag to select TCP vs HTTP. Or remove and add an
issue tag.

---

### 20. There are no automated tests
**Where:** Repository root — no `test/` directory

PlatformIO supports `pio test` with on-host (`native`) and on-target
environments. Critical correctness paths (TLV encode/decode, packet
framing, hash dedup, fragment reassembly, Noise round-trip, Ed25519
self-test) are currently only verifiable by manual testing.

**Fix directive:** Add a `test/` directory and a `[env:native]` block to
`platformio.ini`. Start with:
- `test_tlv` — encode then `_tlv_find` for every defined TLV type.
- `test_noise` — full XX handshake initiator↔responder in-memory.
- `test_ed25519` — RFC 8032 test vectors 1–4.
- `test_fragment` — split + reassemble a 1.5 KB packet, including
  out-of-order arrival.
- `test_dedup` — ring buffer collision and TTL-expiry behaviour.
- `test_bridge_echo` — confirm that messages we sent don't re-enter
  via `_on_bitchat_message`.

Wire `pio test -e native` into CI (GitHub Actions).

---

### 21. Duplicated packet-build code across send paths
**Where:** `_send_text`, `_send_text_as`, `_send_announce`,
`_send_packet`, `_send_encrypted`, `_send_handshake_packet`, `_send_leave`,
`_send_fragmented` all hand-roll the same header layout.

Any future change to the wire format (e.g. v2 header rollout) means
auditing seven near-identical 20-line blocks.

**Fix directive:** Introduce a `BitchatPacketBuilder` helper that:
- Reserves space for the header
- Appends sender_id, payload, signature in order
- Owns the timestamp / TTL / version constants
- Returns a single buffer + length

Refactor every send path through it. Cuts roughly 200 lines.

---

### 22. Dead-code path for nickname in `_handle_message`
**Where:** `src/bitchat/bitchat_ble.cpp:1004-1017`

The fallback "try inline nickname TLV" branch looks up
`BITCHAT_TLV_NICKNAME` (type 0x01) in a `PKT_MESSAGE` payload. By
protocol contract, NICKNAME only appears in `PKT_ANNOUNCE` payloads;
`PKT_MESSAGE` carries `BITCHAT_TLV_TEXT` (0x05). This branch is
dead code that misleads readers.

**Fix directive:** Delete the inline-nickname fallback; the hex-fingerprint
fallback alone is sufficient when the peer's announce hasn't arrived yet.

---

### 23. Magic numbers / hard-coded buffer sizes proliferate
**Where:** Various — e.g. `packet[2048]` (8×), `plaintext[512]` (2×),
`bridged_text[512]`, `PendingChunk::text[240]`, etc.

The 2048/512/240 constants are conceptually tied to
`BITCHAT_BLE_MTU`, `MESH_DATA_MAX`, `BITCHAT_MAX_TEXT_LEN`, etc., but
the binding is implicit.

**Fix directive:** Replace each `[2048]` with a named constant
(`BITCHAT_MAX_PACKET_SIZE`), add `static_assert` invariants, and document
the rationale at the definition site in `config.h`.

---

### 24. `mDNS` lookup uses a 5-second blocking call inside `connect()`
**Where:** `src/meshtastic/meshtastic_tcp.cpp:199`

`MDNS.queryHost(hostname, 5000)` blocks the entire main task. While
acceptable on first boot, it also fires on every reconnect attempt — so
a brief network glitch costs at least 5 s of dead time per cycle.

**Fix directive:** Cache the resolved IP and only re-resolve if a
connection attempt fails. Move the resolve into a non-blocking state
machine if it becomes a hot path.

---

### 25. NVS persistence has no shutdown flush
**Where:** `src/bridge/identity_mapper.cpp:103-108`

`_save_if_needed` debounces writes to once-per-`IDENTITY_SAVE_DEBOUNCE_MS`
(30 s). The very last update sits in RAM until the next *future* update
forces another save — which on a quiet day may never come.

**Fix directive:**
- Add an explicit `IdentityMapper::flush()` and call it from
  `BridgeManager::loop()` whenever `(millis() - _last_save_ms) >
  IDENTITY_SAVE_DEBOUNCE_MS && _dirty`.
- Register an `esp_register_shutdown_handler` to call `flush()` on
  brownout / restart.

---

### 26. No runtime configuration interface
**Where:** Everywhere — config is compile-time only

Every change (WiFi, host, forwarding flags) requires reflash. Field
deployment is painful: lose your WiFi password, lose the bridge.

**Fix directive:** Add a minimal serial-command shell (`set ssid foo`,
`set password bar`, `save`, `reboot`, `status`) that persists to NVS.
Bonus: expose the same over the AP-mode captive portal.

---

### 27. Status reporting is verbose-only and not machine-parseable
**Where:** `src/main.cpp:155-171`

Every 30 s we dump a free-form log line. There's no way to scrape
metrics (peer count, dedup hits, memory, retries) for monitoring.

**Fix directive:** Add an optional JSON status line behind a config
flag, and/or expose stats over a tiny HTTP endpoint in AP mode.

---

### 28. `_relay_is_dup` is computed but never gates initial reception
**Where:** `src/bitchat/bitchat_ble.cpp:1357-1362`

We dedup at *relay* time but still hand every duplicate copy of an
announce or message to `_handle_*`, which then fires `_on_message` and
triggers a duplicate bridge forward. The `BridgeManager` has its own
dedup, so the visible effect is "two log lines per dup, one bridged
output" — wasteful but not broken. Still: O(M·N) work per duplicate.

**Fix directive:** Move the `_relay_is_dup` check up before
`_handle_message` / `_handle_announce` dispatch in `_process_incoming`,
not just inside `_relay_packet`. Skip both delivery and relay for
duplicates.

---

### 29. The Wokwi simulator config and `diagram.json` exist but aren't documented
**Where:** `wokwi.toml`, `diagram.json`

The README has no instruction for running on Wokwi, which is the easiest
zero-hardware way to validate changes.

**Fix directive:** Add a "Simulation" section to the README documenting
the Wokwi quickstart, with links to the public project URL.

---

### 30. Heavy use of `Serial.printf` from BLE callback context
**Where:** `src/bitchat/bitchat_ble.cpp:312` and similar

`onDisconnect` and the notify subscribe lambda execute on the BLE host
task, not the main task. `Serial.printf` is generally thread-safe in
arduino-esp32, but it can still cause priority inversion (BLE task
blocked on UART). Combined with bug #18 above, this is a latency risk.

**Fix directive:** Push log lines from BLE-task contexts into a SPSC
ring buffer drained by the main loop, the same way packets are handled.

---

## Suggested ordering of work

1. **Wire-format and correctness first** — items 1–5 (echo loop, announce
   dedup, TTL/sig, fragment sizing, AAD mismatch). These produce visible
   bugs and shape the protocol layer.
2. **Security hardening** — items 6–10. Each is small, but together they
   close the door on impersonation and replay.
3. **Robustness** — items 11–18. These improve operator experience without
   changing wire format.
4. **Tests and quality** — items 19–30. Bake tests into CI *before*
   landing further protocol changes so we can verify with confidence.

Each item should land as its own small PR with at least one targeted test
in `test/native/`. Avoid omnibus refactors — the protocol is hard enough
to reason about one layer at a time.
