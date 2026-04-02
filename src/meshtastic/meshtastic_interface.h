#pragma once

#include <functional>
#include "../bridge/message.h"

// Abstract interface for communicating with a Meshtastic node.
// Concrete implementations: MeshtasticTCP (Phase 1), MeshtasticHTTP (future).

class MeshtasticInterface {
public:
    using MessageCallback = std::function<void(const BridgeMessage &msg)>;

    virtual ~MeshtasticInterface() = default;

    // Connect to the Meshtastic node. Returns true on success.
    virtual bool connect() = 0;

    // Disconnect cleanly.
    virtual void disconnect() = 0;

    // Returns true if currently connected and config handshake is complete.
    virtual bool is_connected() const = 0;

    // Must be called regularly from the main loop.
    // Reads incoming packets, sends heartbeats, handles reconnection.
    virtual void loop() = 0;

    // Send a text message to the Meshtastic mesh.
    // Returns true if the message was queued/sent successfully.
    virtual bool send_text(const char *text, uint32_t dest = 0xFFFFFFFF, uint8_t channel = 0) = 0;

    // Register a callback for incoming text messages.
    void on_message(MessageCallback cb) { _on_message = cb; }

protected:
    MessageCallback _on_message;
};
