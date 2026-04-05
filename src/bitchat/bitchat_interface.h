#pragma once

#include <functional>
#include "../bridge/message.h"

struct BitchatKeypair;  // forward declaration

// Abstract interface for communicating with the Bitchat BLE mesh.
// Concrete implementation: BitchatBLE.

class BitchatInterface {
public:
    using MessageCallback = std::function<void(const BridgeMessage &msg)>;

    virtual ~BitchatInterface() = default;

    // Initialize BLE and start advertising/scanning. Returns true on success.
    virtual bool begin() = 0;

    // Stop BLE operations.
    virtual void end() = 0;

    // Returns true if BLE is initialized and operational.
    virtual bool is_active() const = 0;

    // Must be called regularly from the main loop.
    // Handles scanning, connection management, and incoming packets.
    virtual void loop() = 0;

    // Send a text message to the Bitchat BLE mesh (as the bridge identity).
    // Returns true if the message was broadcast successfully.
    virtual bool send_text(const char *text) = 0;

    // Send a text message as a virtual identity (Option A: plaintext only).
    // The keypair determines sender_id and signature.
    virtual bool send_text_as(const char *text, const BitchatKeypair *identity) = 0;

    // Send an announce packet as a virtual identity to all peers.
    virtual void announce_virtual(const BitchatKeypair *identity, const char *name) = 0;

    // Register a callback for incoming text messages.
    void on_message(MessageCallback cb) { _on_message = cb; }

protected:
    MessageCallback _on_message;
};
