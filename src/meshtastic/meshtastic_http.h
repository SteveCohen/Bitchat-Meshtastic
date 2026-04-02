#pragma once

#include "meshtastic_interface.h"

// ── HTTP Interface Placeholder ───────────────────────────────────────
//
// Meshtastic nodes also expose an HTTP API (typically on port 80/443):
//   GET  /api/v1/fromradio   — poll for incoming FromRadio messages
//   PUT  /api/v1/toradio     — send a ToRadio message (body = protobuf bytes)
//
// This is simpler than TCP (no framing, no persistent connection) but
// requires polling. Implement when TCP is not available or for simpler setups.
//
// The same mesh_proto.h encode/decode helpers work here — just the
// transport layer changes.

class MeshtasticHTTP : public MeshtasticInterface {
public:
    MeshtasticHTTP(const char *base_url = "http://192.168.1.100") {
        (void)base_url;
        // TODO: store base_url, set up HTTPClient
    }

    bool connect() override {
        // TODO: GET /api/v1/fromradio with want_config_id
        return false;
    }

    void disconnect() override {
        // TODO: PUT /api/v1/toradio with disconnect
    }

    bool is_connected() const override { return false; }

    void loop() override {
        // TODO: poll GET /api/v1/fromradio periodically
    }

    bool send_text(const char *text, uint32_t dest = 0xFFFFFFFF, uint8_t channel = 0) override {
        (void)text; (void)dest; (void)channel;
        // TODO: PUT /api/v1/toradio with encoded ToRadio
        return false;
    }
};
