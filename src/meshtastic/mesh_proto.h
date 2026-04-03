#pragma once

#include <cstdint>
#include <cstring>
#include <esp_random.h>
#include "../config.h"

// ── Minimal hand-rolled protobuf encoder/decoder ─────────────────────
//
// We only need a tiny subset of the Meshtastic protobuf schema:
//   ToRadio { packet: MeshPacket | want_config_id: uint32 | heartbeat }
//   FromRadio { id: uint32, packet: MeshPacket | config_complete_id: uint32 }
//   MeshPacket { from, to, channel, decoded: Data, id, hop_limit }
//   Data { portnum: uint32, payload: bytes }
//
// Rather than pulling in nanopb + full .proto compilation, we encode/decode
// the specific wire formats by hand. This keeps the build simple and the
// binary small.
//
// Protobuf wire format refresher:
//   field_tag = (field_number << 3) | wire_type
//   wire_type 0 = varint, 2 = length-delimited, 5 = fixed32

namespace mesh_proto {

// ── Varint helpers ────────────────────────────────────

inline int encode_varint(uint8_t *buf, uint32_t value) {
    int i = 0;
    while (value > 0x7F) {
        buf[i++] = (value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[i++] = value & 0x7F;
    return i;
}

inline int decode_varint(const uint8_t *buf, int len, uint32_t *value) {
    *value = 0;
    int shift = 0;
    for (int i = 0; i < len && i < 5; i++) {
        *value |= (uint32_t)(buf[i] & 0x7F) << shift;
        shift += 7;
        if (!(buf[i] & 0x80)) return i + 1;
    }
    return -1; // malformed
}

// ── Tag helpers ───────────────────────────────────────

inline int encode_tag(uint8_t *buf, uint32_t field_num, uint8_t wire_type) {
    return encode_varint(buf, (field_num << 3) | wire_type);
}

inline int encode_fixed32(uint8_t *buf, uint32_t field_num, uint32_t value) {
    int n = encode_tag(buf, field_num, 5); // wire_type 5 = fixed32
    memcpy(buf + n, &value, 4);
    return n + 4;
}

inline int encode_varint_field(uint8_t *buf, uint32_t field_num, uint32_t value) {
    int n = encode_tag(buf, field_num, 0); // wire_type 0 = varint
    n += encode_varint(buf + n, value);
    return n;
}

inline int encode_bytes_field(uint8_t *buf, uint32_t field_num, const uint8_t *data, int data_len) {
    int n = encode_tag(buf, field_num, 2); // wire_type 2 = length-delimited
    n += encode_varint(buf + n, data_len);
    memcpy(buf + n, data, data_len);
    return n + data_len;
}

// ── Data submessage (portnum + payload) ───────────────

// Encode Data { portnum: field 1, payload: field 2 }
inline int encode_data(uint8_t *buf, uint32_t portnum, const uint8_t *payload, int payload_len) {
    int n = 0;
    n += encode_varint_field(buf + n, 1, portnum);                      // portnum
    n += encode_bytes_field(buf + n, 2, payload, payload_len);          // payload
    return n;
}

// ── MeshPacket encoding ───────────────────────────────

// Encode MeshPacket { from:1, to:2, channel:3, decoded:4, id:6, hop_limit:9 }
inline int encode_mesh_packet(uint8_t *buf, uint32_t to, uint32_t id,
                               uint8_t channel, uint8_t hop_limit,
                               const uint8_t *data_buf, int data_len) {
    int n = 0;
    // to (field 2, fixed32)
    n += encode_fixed32(buf + n, 2, to);
    // channel (field 3, varint)
    if (channel != 0) {
        n += encode_varint_field(buf + n, 3, channel);
    }
    // decoded = Data (field 4, length-delimited submessage)
    n += encode_tag(buf + n, 4, 2);
    n += encode_varint(buf + n, data_len);
    memcpy(buf + n, data_buf, data_len);
    n += data_len;
    // id (field 6, fixed32)
    n += encode_fixed32(buf + n, 6, id);
    // hop_limit (field 9, varint)
    n += encode_varint_field(buf + n, 9, hop_limit);
    return n;
}

// ── ToRadio encoding ──────────────────────────────────

// ToRadio { packet: field 1 }
inline int encode_to_radio_packet(uint8_t *buf, const uint8_t *mesh_packet, int mp_len) {
    int n = 0;
    n += encode_tag(buf + n, 1, 2);
    n += encode_varint(buf + n, mp_len);
    memcpy(buf + n, mesh_packet, mp_len);
    n += mp_len;
    return n;
}

// ToRadio { want_config_id: field 3 }
inline int encode_to_radio_want_config(uint8_t *buf, uint32_t config_id) {
    int n = 0;
    n += encode_varint_field(buf + n, 3, config_id);
    return n;
}

// ToRadio { heartbeat: field 7 } (empty submessage)
inline int encode_to_radio_heartbeat(uint8_t *buf) {
    int n = 0;
    n += encode_tag(buf + n, 7, 2);
    buf[n++] = 0; // zero-length submessage
    return n;
}

// ── Protobuf field decoder ────────────────────────────

struct ProtoField {
    uint32_t field_num;
    uint8_t wire_type;
    union {
        uint32_t varint_val;
        struct {
            const uint8_t *data;
            int len;
        } bytes_val;
        uint32_t fixed32_val;
    };
};

// Iterate through protobuf fields in a buffer.
// Returns number of bytes consumed, or -1 on error.
inline int decode_field(const uint8_t *buf, int buf_len, ProtoField *field) {
    if (buf_len <= 0) return -1;

    uint32_t tag;
    int pos = decode_varint(buf, buf_len, &tag);
    if (pos < 0) return -1;

    field->field_num = tag >> 3;
    field->wire_type = tag & 0x07;

    switch (field->wire_type) {
        case 0: { // varint
            int n = decode_varint(buf + pos, buf_len - pos, &field->varint_val);
            if (n < 0) return -1;
            return pos + n;
        }
        case 2: { // length-delimited
            uint32_t len;
            int n = decode_varint(buf + pos, buf_len - pos, &len);
            if (n < 0 || pos + n + (int)len > buf_len) return -1;
            field->bytes_val.data = buf + pos + n;
            field->bytes_val.len = (int)len;
            return pos + n + (int)len;
        }
        case 5: { // fixed32
            if (pos + 4 > buf_len) return -1;
            memcpy(&field->fixed32_val, buf + pos, 4);
            return pos + 4;
        }
        case 1: { // fixed64 — skip 8 bytes
            if (pos + 8 > buf_len) return -1;
            return pos + 8;
        }
        default:
            return -1; // unknown wire type
    }
}

// ── Parsed text message from FromRadio ────────────────

struct ParsedTextMessage {
    bool valid = false;
    uint32_t from_node = 0;
    uint32_t to_node = 0;
    uint32_t packet_id = 0;
    uint8_t channel = 0;
    char text[256] = {};
};

// Parse a FromRadio payload looking for a text message.
// Returns a ParsedTextMessage with valid=true if found.
inline ParsedTextMessage parse_from_radio(const uint8_t *buf, int len) {
    ParsedTextMessage result;

    // FromRadio: look for field 2 (packet) or field 7 (config_complete_id)
    int pos = 0;
    const uint8_t *packet_data = nullptr;
    int packet_len = 0;

    while (pos < len) {
        ProtoField f;
        int consumed = decode_field(buf + pos, len - pos, &f);
        if (consumed < 0) break;
        pos += consumed;

        if (f.field_num == 2 && f.wire_type == 2) {
            packet_data = f.bytes_val.data;
            packet_len = f.bytes_val.len;
        }
    }

    if (!packet_data) return result;

    // Parse MeshPacket: from(1,fixed32), to(2,fixed32), channel(3,varint), decoded(4,bytes), id(6,fixed32)
    const uint8_t *decoded_data = nullptr;
    int decoded_len = 0;
    pos = 0;

    while (pos < packet_len) {
        ProtoField f;
        int consumed = decode_field(packet_data + pos, packet_len - pos, &f);
        if (consumed < 0) break;
        pos += consumed;

        switch (f.field_num) {
            case 1: if (f.wire_type == 5) result.from_node = f.fixed32_val; break;
            case 2: if (f.wire_type == 5) result.to_node = f.fixed32_val; break;
            case 3: if (f.wire_type == 0) result.channel = (uint8_t)f.varint_val; break;
            case 4: if (f.wire_type == 2) { decoded_data = f.bytes_val.data; decoded_len = f.bytes_val.len; } break;
            case 6: if (f.wire_type == 5) result.packet_id = f.fixed32_val; break;
        }
    }

    if (!decoded_data) return result;

    // Parse Data: portnum(1,varint), payload(2,bytes)
    uint32_t portnum = 0;
    const uint8_t *payload = nullptr;
    int payload_len = 0;
    pos = 0;

    while (pos < decoded_len) {
        ProtoField f;
        int consumed = decode_field(decoded_data + pos, decoded_len - pos, &f);
        if (consumed < 0) break;
        pos += consumed;

        switch (f.field_num) {
            case 1: if (f.wire_type == 0) portnum = f.varint_val; break;
            case 2: if (f.wire_type == 2) { payload = f.bytes_val.data; payload_len = f.bytes_val.len; } break;
        }
    }

    if (portnum != PORTNUM_TEXT_MESSAGE_APP || !payload || payload_len == 0) {
        return result;
    }

    // Copy text (truncate if needed)
    int copy_len = (payload_len < (int)sizeof(result.text) - 1) ? payload_len : (int)sizeof(result.text) - 1;
    memcpy(result.text, payload, copy_len);
    result.text[copy_len] = '\0';
    result.valid = true;
    return result;
}

// Check if a FromRadio contains config_complete_id matching our nonce
inline bool is_config_complete(const uint8_t *buf, int len, uint32_t expected_nonce) {
    int pos = 0;
    while (pos < len) {
        ProtoField f;
        int consumed = decode_field(buf + pos, len - pos, &f);
        if (consumed < 0) break;
        pos += consumed;
        // config_complete_id = field 7, varint
        if (f.field_num == 7 && f.wire_type == 0 && f.varint_val == expected_nonce) {
            return true;
        }
    }
    return false;
}

// ── Packet ID generator ──────────────────────────────

inline uint32_t generate_packet_id(uint32_t *counter) {
    uint32_t seq = ((*counter) + 1) & 0x3FF;          // lower 10 bits: sequential
    uint32_t rand_part = (esp_random() & 0x3FFFFF) << 10; // upper 22 bits: random
    *counter = seq | rand_part;
    return *counter;
}

} // namespace mesh_proto
