#pragma once

#include <WiFi.h>
#include <ESPmDNS.h>
#include "meshtastic_interface.h"
#include "mesh_proto.h"
#include "../config.h"

class MeshtasticTCP : public MeshtasticInterface {
public:
    MeshtasticTCP(const char *host = MESHTASTIC_HOST, uint16_t port = MESHTASTIC_PORT);

    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;
    void loop() override;
    bool send_text(const char *text, uint32_t dest = MESH_BROADCAST, uint8_t channel = MESHTASTIC_CHANNEL) override;

private:
    // TCP connection
    WiFiClient _client;
    const char *_host;
    uint16_t _port;
    IPAddress _resolved_ip;
    bool _connected = false;
    bool _config_complete = false;
    uint32_t _config_nonce = 0;
    uint32_t _my_node_id = 0;

    // Packet ID counter
    uint32_t _packet_id_counter = 0;

    // Node directory: maps node IDs to user names (learned during config)
    struct NodeEntry {
        uint32_t id = 0;
        char long_name[40] = {};
        char short_name[5] = {};
    };
    static constexpr int MAX_NODES = 32;
    NodeEntry _nodes[MAX_NODES] = {};
    int _node_count = 0;

    void _store_node_info(const mesh_proto::ParsedNodeInfo &ni);

public:
    // Look up a node's long_name by ID. Returns nullptr if unknown.
    const char* get_node_name(uint32_t node_id) const;
    uint32_t my_node_id() const { return _my_node_id; }

private:

    // Heartbeat tracking
    unsigned long _last_heartbeat_ms = 0;

    // Receive buffer + state machine
    uint8_t _rx_buf[MESH_HEADER_LEN + MESH_MAX_PAYLOAD];
    int _rx_pos = 0;
    enum RxState { WAIT_START1, WAIT_START2, READ_LEN_HI, READ_LEN_LO, READ_PAYLOAD };
    RxState _rx_state = WAIT_START1;
    uint16_t _rx_payload_len = 0;

    // Internal methods
    bool _do_handshake();
    bool _resolve_host();  // Resolve mDNS .local hostname to IP
    void _send_raw(const uint8_t *payload, uint16_t len);
    void _process_byte(uint8_t b);
    void _handle_from_radio(const uint8_t *buf, int len);
    void _send_heartbeat();
};
