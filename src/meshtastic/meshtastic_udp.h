#pragma once

#include <WiFi.h>
#include <WiFiUdp.h>
#include "meshtastic_interface.h"
#include "mesh_proto.h"
#include "../config.h"

// MeshtasticUDP — joins the Meshtastic UDP-multicast group (224.0.0.69:4403
// by default) and acts as a peer on the mesh. No TCP API session is held,
// so multiple consumers can coexist with the bridge.
//
// Wire format per upstream firmware src/mesh/udp/UdpMulticastHandler.h:
// each datagram is a raw `MeshPacket` protobuf with the SubPacket carried
// in the `encrypted` field (22). Channel-key AES-CTR is used to encrypt
// the inner Data submessage; key is the LongFast PSK by default.
//
// Caveats vs. the TCP backend:
// - No node-directory dump at startup; names populate lazily as we observe
//   NodeInfo packets.
// - We pick our own node_num (derived from the BLE MAC if the user
//   doesn't override `BRIDGE_MESH_NODE_NUM`).
// - `is_connected()` reports WiFi-up + multicast-listener-up, not a
//   handshake state.

class MeshtasticUDP : public MeshtasticInterface {
public:
    MeshtasticUDP() = default;

    bool connect() override;
    void disconnect() override;
    bool is_connected() const override;
    void loop() override;
    bool send_text(const char *text,
                   uint32_t dest = MESH_BROADCAST,
                   uint8_t channel = MESHTASTIC_CHANNEL) override;
    bool send_node_info(uint32_t bridge_node_num,
                         const char *long_name,
                         const char *short_name) override;

    // Used by main.cpp / BridgeManager status prints.
    uint32_t my_node_id() const { return _my_node_num; }

private:
    mutable WiFiUDP _udp;
    bool      _running     = false;
    uint32_t  _my_node_num = 0;
    uint32_t  _packet_id_counter = 0;
    uint8_t   _psk[32]     = {};
    size_t    _psk_len     = 0;

    // Bridge-as-node identity (set via send_node_info).
    char      _bridge_long_name[40]  = {};
    char      _bridge_short_name[5]  = {};
    uint32_t  _last_nodeinfo_ms      = 0;

    // Recent self-sent packet IDs — drop loopback datagrams the WiFi
    // stack delivers back to us when we're a member of the multicast
    // group we just sent to.
    static constexpr int LOOPBACK_RING = 16;
    uint32_t  _self_ids[LOOPBACK_RING] = {};
    int       _self_ids_idx            = 0;

    bool _send_packet(uint32_t to, uint8_t channel, uint8_t hop_limit,
                      const uint8_t *data_buf, int data_len);
    void _process_datagram(const uint8_t *buf, int len, IPAddress src);
    bool _is_self_id(uint32_t id) const;
    void _record_self_id(uint32_t id);
    void _derive_node_num_from_mac();
    bool _load_psk_from_hex(const char *hex);
};
