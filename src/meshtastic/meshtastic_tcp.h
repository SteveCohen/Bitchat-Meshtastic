#pragma once

#include <WiFi.h>
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
    bool _connected = false;
    bool _config_complete = false;
    uint32_t _config_nonce = 0;
    uint32_t _my_node_id = 0;

    // Packet ID counter
    uint32_t _packet_id_counter = 0;

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
    void _send_raw(const uint8_t *payload, uint16_t len);
    void _process_byte(uint8_t b);
    void _handle_from_radio(const uint8_t *buf, int len);
    void _send_heartbeat();
};
