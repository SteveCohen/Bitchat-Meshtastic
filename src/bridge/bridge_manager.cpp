#include "bridge_manager.h"
#include <Arduino.h>

static const char *TAG = "Bridge";

BridgeManager::BridgeManager(MeshtasticInterface &mesh, BitchatInterface &bitchat,
                             const BitchatKeypair *master_kp)
    : _mesh(mesh), _bitchat(bitchat), _master_kp(master_kp) {}

bool BridgeManager::begin() {
    Serial.printf("[%s] Starting bridge...\n", TAG);

    _id_mapper.load();

    // Register eviction callback for farewell notifications
    _virt_registry.set_evict_callback([this](const VirtualIdentity &vi) {
        _on_identity_evicted(vi);
    });

    // Wire up callbacks
    _mesh.on_message([this](const BridgeMessage &msg) {
        _on_meshtastic_message(msg);
    });

    _bitchat.on_message([this](const BridgeMessage &msg) {
        _on_bitchat_message(msg);
    });

    // Initialize Bitchat BLE
    if (!_bitchat.begin()) {
        Serial.printf("[%s] Bitchat BLE init failed\n", TAG);
        return false;
    }

    // Connect to Meshtastic (may fail if WiFi isn't ready yet — that's ok,
    // loop() will retry)
    if (!_mesh.connect()) {
        Serial.printf("[%s] Meshtastic TCP connect failed (will retry)\n", TAG);
    }

    Serial.printf("[%s] Bridge started\n", TAG);
    return true;
}

void BridgeManager::loop() {
    // Drive both interfaces
    _mesh.loop();
    _bitchat.loop();

    // Non-blocking re-announce of active virtual identities (one per tick)
    if (_master_kp) {
        if (_virt_announce_idx < 0) {
            // Idle: start a new cycle when interval has elapsed
            if ((millis() - _last_virt_announce_cycle_ms) > BITCHAT_ANNOUNCE_INTERVAL_MS) {
                _virt_announce_idx = 0;
                _last_virt_announce_cycle_ms = millis();
                _last_virt_announce_step_ms = 0;  // send first immediately
            }
        }
        if (_virt_announce_idx >= 0 &&
            (millis() - _last_virt_announce_step_ms) >= VIRTUAL_ANNOUNCE_STAGGER_MS) {
            const auto &all = _virt_registry.all();
            if (_virt_announce_idx < (int)all.size()) {
                const auto &vi = all[_virt_announce_idx];
                _bitchat.announce_virtual(&vi.keypair,
                    vi.display_name[0] ? vi.display_name : nullptr);
                _last_virt_announce_step_ms = millis();
                _virt_announce_idx++;
            } else {
                _virt_announce_idx = -1;  // cycle complete
            }
        }
    }

    // Periodic memory pressure check — shed identities if heap is low
    if (millis() - _last_mem_check_ms > VIRT_ID_MEMORY_CHECK_MS) {
        _last_mem_check_ms = millis();
        _virt_registry.check_memory_pressure();
    }

    // Reconnect Meshtastic with exponential backoff
    if (!_mesh.is_connected()) {
        if (_mesh_was_connected) _mesh_was_connected = false;
        if (millis() - _mesh_retry_ms > _mesh_retry_interval) {
            _mesh_retry_ms = millis();
            Serial.printf("[%s] Reconnecting to Meshtastic (backoff %ds)...\n",
                          TAG, (int)(_mesh_retry_interval / 1000));
            if (_mesh.connect()) {
                _mesh_retry_interval = 5000;  // reset on success
            } else {
                // Double backoff, cap at 5 minutes
                _mesh_retry_interval = (_mesh_retry_interval < 300000)
                    ? _mesh_retry_interval * 2 : 300000;
            }
        }
    } else {
        _mesh_retry_interval = 5000;  // reset while connected
        // Rising edge: publish our NodeInfo so the phone app sees the bridge.
        if (!_mesh_was_connected) {
            _mesh_was_connected = true;
            _publish_bridge_node_info();
        }
    }

    // Drain any queued long-message chunks (non-blocking).
    _drain_pending_chunks();
}

// ── Bridge NodeInfo publication ──────────────────────

void BridgeManager::_publish_bridge_node_info() {
    if (!_master_kp) return;
    // Derive a 4-byte node_num from the bitchat fingerprint. Mask high bit
    // (Meshtastic node IDs are 32-bit but the phone app sometimes treats
    // negative-looking values oddly) and avoid 0 / broadcast.
    uint32_t node_num = 0;
    for (int i = 0; i < 4; i++) {
        node_num |= (uint32_t)_master_kp->fingerprint[i] << (i * 8);
    }
    node_num &= 0x7FFFFFFFu;
    if (node_num == 0)              node_num = 1;
    if (node_num == MESH_BROADCAST) node_num ^= 0x12345678u;
    _mesh.send_node_info(node_num, BRIDGE_NAME, BRIDGE_SHORT_NAME);
}

// ── Message handlers ─────────────────────────────────

void BridgeManager::_on_meshtastic_message(const BridgeMessage &msg) {
    uint32_t h = msg.hash();
    if (_is_duplicate(h)) {
        Serial.printf("[%s] Dedup: skipping Meshtastic message\n", TAG);
        return;
    }
    _record_hash(h);

    // Register sender name in identity mapper (from MeshtasticTCP's NodeInfo)
    if (msg.sender.meshtastic_node_id && msg.sender.display_name[0]) {
        _id_mapper.update_mesh_name(msg.sender.meshtastic_node_id,
                                     msg.sender.display_name, nullptr);
    }

    // Try to route through a virtual identity for this Meshtastic sender
    if (_master_kp && msg.sender.meshtastic_node_id) {
        VirtualIdentity *vi = _virt_registry.get_or_create(
            msg.sender.meshtastic_node_id, _master_kp);

        if (vi) {
            // Update the virtual identity's display name
            if (msg.sender.display_name[0] && !vi->display_name[0]) {
                strncpy(vi->display_name, msg.sender.display_name,
                        sizeof(vi->display_name) - 1);
                // Announce this new virtual identity to all BLE peers
                _bitchat.announce_virtual(&vi->keypair, vi->display_name);
            }

            // Send as the virtual identity (just the raw message text, no prefix)
            Serial.printf("[%s] Mesh→BLE (virtual %s): %s\n", TAG,
                          vi->display_name[0] ? vi->display_name : "?", msg.text);

            BridgeMessage outgoing;
            strncpy(outgoing.text, msg.text, sizeof(outgoing.text) - 1);
            _record_hash(outgoing.hash());

            _bitchat.send_text_as(msg.text, &vi->keypair);
            _msg_count++;
            return;
        }
    }

    // Fallback: send as bridge identity with prefix
    char bridged_text[512];
    _format_bridged_text(msg, bridged_text, sizeof(bridged_text));

    Serial.printf("[%s] Mesh→BLE: %s\n", TAG, bridged_text);

    BridgeMessage outgoing;
    strncpy(outgoing.text, bridged_text, sizeof(outgoing.text) - 1);
    _record_hash(outgoing.hash());

    _bitchat.send_text(bridged_text);
    _msg_count++;
}

void BridgeManager::_on_bitchat_message(const BridgeMessage &msg) {
    uint32_t h = msg.hash();
    if (_is_duplicate(h)) {
        Serial.printf("[%s] Dedup: skipping Bitchat message\n", TAG);
        return;
    }
    _record_hash(h);

    // Register sender name in identity mapper (from bitchat announce nickname)
    if (msg.sender.display_name[0] && msg.sender.has_bitchat_fingerprint()) {
        _id_mapper.update_ble_name(msg.sender.bitchat_fingerprint,
                                    msg.sender.display_name);
    }

    // Format the message for Meshtastic
    char bridged_text[512];
    _format_bridged_text(msg, bridged_text, sizeof(bridged_text));

    Serial.printf("[%s] BLE→Mesh: %s\n", TAG, bridged_text);

    // If the formatted text fits in a single Meshtastic packet, send it
    // directly. Otherwise split into "(N/M):" chunks queued for emission
    // from loop() with an inter-chunk delay.
    int total_len = (int)strlen(bridged_text);
    if (total_len <= MESH_DATA_MAX) {
        BridgeMessage outgoing;
        strncpy(outgoing.text, bridged_text, sizeof(outgoing.text) - 1);
        _record_hash(outgoing.hash());

        _mesh.send_text(bridged_text);
        _msg_count++;
        return;
    }

    // Long path: separate the "[B] sender: " prefix from the body so each
    // chunk can carry the sender attribution + a "(i/M)" indicator.
    const char *sep = strstr(bridged_text, ": ");
    char sender_prefix[80];
    const char *body;
    if (sep) {
        int plen = (int)(sep - bridged_text);
        if (plen >= (int)sizeof(sender_prefix)) plen = sizeof(sender_prefix) - 1;
        memcpy(sender_prefix, bridged_text, plen);
        sender_prefix[plen] = '\0';
        body = sep + 2;
    } else {
        // No "sender: " in formatted text — fall back to a generic prefix.
        snprintf(sender_prefix, sizeof(sender_prefix), "%s",
                 (msg.origin == MessageOrigin::MESHTASTIC) ? MSG_PREFIX_MESH : MSG_PREFIX_BLE);
        body = bridged_text;
    }

    int n_chunks = _split_into_chunks(sender_prefix, body);
    Serial.printf("[%s] BLE→Mesh chunked %d/%d (total %d bytes)\n",
                  TAG, n_chunks, n_chunks, total_len);
    _msg_count++;
}

// Split a long body into chunks. Each chunk is "{prefix} (i/N): {slice}",
// sized so the whole chunk fits in MESH_DATA_MAX. UTF-8 boundaries are
// respected (we walk back to a lead byte before splitting). Trailing
// content beyond BRIDGE_CHUNK_MAX_CHUNKS is dropped with a "…" marker.
int BridgeManager::_split_into_chunks(const char *sender_prefix,
                                       const char *body) {
    // First decide how many chunks we'll need, given the per-chunk overhead.
    int sender_len = (int)strlen(sender_prefix);
    // " (i/M): " is at most 9 chars (e.g. " (4/4): ").
    int prefix_overhead = sender_len + 9;
    int max_slice = MESH_DATA_MAX - prefix_overhead;
    if (max_slice < 16) max_slice = 16;

    int body_len = (int)strlen(body);
    int needed = (body_len + max_slice - 1) / max_slice;
    if (needed > BRIDGE_CHUNK_MAX_CHUNKS) needed = BRIDGE_CHUNK_MAX_CHUNKS;
    if (needed < 1) needed = 1;

    int pos = 0;
    for (int i = 0; i < needed; i++) {
        int remaining = body_len - pos;
        bool last = (i == needed - 1);
        bool will_truncate = false;
        int take = remaining;
        if (i < BRIDGE_CHUNK_MAX_CHUNKS - 1 && remaining > max_slice) {
            take = max_slice;
            // Walk back to a UTF-8 lead byte boundary so we don't bisect
            // a multi-byte char. Continuation bytes are 0b10xxxxxx.
            while (take > 0 && (unsigned char)body[pos + take] >= 0x80
                            && (unsigned char)body[pos + take] < 0xC0) {
                take--;
            }
            if (take == 0) take = max_slice; // pathological — fall through
        } else if (remaining > max_slice) {
            // Last chunk and body still larger → truncate.
            take = max_slice - 4;  // leave room for " ..."
            while (take > 0 && (unsigned char)body[pos + take] >= 0x80
                            && (unsigned char)body[pos + take] < 0xC0) {
                take--;
            }
            if (take <= 0) take = max_slice - 4;
            will_truncate = true;
        }
        char *out = _pending_chunks[i].text;
        if (will_truncate) {
            snprintf(out, sizeof(_pending_chunks[i].text),
                     "%s (%d/%d): %.*s ...",
                     sender_prefix, i + 1, needed, take, body + pos);
        } else {
            snprintf(out, sizeof(_pending_chunks[i].text),
                     "%s (%d/%d): %.*s",
                     sender_prefix, i + 1, needed, take, body + pos);
        }
        // Record dedup hash so the chunk doesn't echo back through the bridge.
        BridgeMessage tmp;
        strncpy(tmp.text, out, sizeof(tmp.text) - 1);
        _record_hash(tmp.hash());

        pos += take;
        if (last) break;
    }

    _pending_total      = needed;
    _pending_next       = 0;
    _pending_send_at_ms = millis();  // first chunk goes immediately
    return needed;
}

void BridgeManager::_drain_pending_chunks() {
    if (_pending_next >= _pending_total) return;
    if ((int32_t)(millis() - _pending_send_at_ms) < 0) return;

    if (!_mesh.is_connected()) {
        // Bail rather than spin: drop the queue, log the loss.
        Serial.printf("[%s] dropping queued chunks (mesh disconnected)\n", TAG);
        _pending_next = _pending_total;
        return;
    }

    const char *txt = _pending_chunks[_pending_next].text;
    Serial.printf("[%s] BLE→Mesh chunk %d/%d: %s\n",
                  TAG, _pending_next + 1, _pending_total, txt);
    _mesh.send_text(txt);
    _pending_next++;
    _pending_send_at_ms = millis() + BRIDGE_CHUNK_DELAY_MS;
}

// ── Deduplication ────────────────────────────────────

bool BridgeManager::_is_duplicate(uint32_t hash) {
    uint32_t now = millis();
    for (int i = 0; i < DEDUP_CACHE_SIZE; i++) {
        if (_dedup[i].hash == hash &&
            (now - _dedup[i].timestamp_ms) < DEDUP_TTL_MS) {
            return true;
        }
    }
    return false;
}

void BridgeManager::_record_hash(uint32_t hash) {
    _dedup[_dedup_idx].hash = hash;
    _dedup[_dedup_idx].timestamp_ms = millis();
    _dedup_idx = (_dedup_idx + 1) % DEDUP_CACHE_SIZE;
}

// ── Message formatting ───────────────────────────────

void BridgeManager::_format_bridged_text(const BridgeMessage &msg, char *out, int out_len) {
    const char *prefix = (msg.origin == MessageOrigin::MESHTASTIC) ? MSG_PREFIX_MESH : MSG_PREFIX_BLE;

    // Resolve sender name: try identity mapper first, then message display_name
    const char *sender = nullptr;
    if (msg.origin == MessageOrigin::MESHTASTIC && msg.sender.meshtastic_node_id) {
        sender = _id_mapper.get_mesh_name(msg.sender.meshtastic_node_id);
    } else if (msg.origin == MessageOrigin::BITCHAT && msg.sender.has_bitchat_fingerprint()) {
        sender = _id_mapper.get_ble_name(msg.sender.bitchat_fingerprint);
    }
    if (!sender) {
        sender = msg.sender.display_name[0] ? msg.sender.display_name : "unknown";
    }

    snprintf(out, out_len, "%s%s: %s", prefix, sender, msg.text);
}

// ── Identity eviction ───────────────────────────────

void BridgeManager::_on_identity_evicted(const VirtualIdentity &vi) {
    const char *name = vi.display_name[0] ? vi.display_name : "a Meshtastic user";

    // Notify Bitchat peers: send a farewell message as the departing identity
    char ble_msg[128];
    snprintf(ble_msg, sizeof(ble_msg),
             "%s has gone idle and left the chat. New messages from them will appear under %s.",
             name, BRIDGE_NAME);

    // Notify Meshtastic: let the user know their identity was released
    char mesh_msg[128];
    snprintf(mesh_msg, sizeof(mesh_msg),
             "[B] Bridge released identity for %s (memory pressure). "
             "Messages will still be bridged under %s.",
             name, BRIDGE_NAME);

    // Record hashes BEFORE sending to prevent echo loops
    BridgeMessage tmp;
    strncpy(tmp.text, ble_msg, sizeof(tmp.text) - 1);
    _record_hash(tmp.hash());
    memset(tmp.text, 0, sizeof(tmp.text));
    strncpy(tmp.text, mesh_msg, sizeof(tmp.text) - 1);
    _record_hash(tmp.hash());

    _bitchat.send_text_as(ble_msg, &vi.keypair);
    _mesh.send_text(mesh_msg);

    Serial.printf("[%s] Evicted virtual identity for %s (node %08x), "
                  "heap=%dKB, remaining=%d\n",
                  TAG, name, vi.mesh_node_id,
                  (int)(esp_get_free_heap_size() / 1024),
                  _virt_registry.count() - 1);
}
