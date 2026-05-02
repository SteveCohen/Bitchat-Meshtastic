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
// `from = 0` means "don't emit the field" — the radio fills it with its own
// node ID. Pass a non-zero `from` (e.g. for bridge-originated NodeInfo) to
// have the packet appear as if it came from a different node.
inline int encode_mesh_packet(uint8_t *buf, uint32_t from, uint32_t to,
                               uint32_t id, uint8_t channel, uint8_t hop_limit,
                               const uint8_t *data_buf, int data_len) {
    int n = 0;
    // from (field 1, fixed32) — only emitted when nonzero
    if (from != 0) {
        n += encode_fixed32(buf + n, 1, from);
    }
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

// Backwards-compatible overload (pre-Feature 1 callers).
inline int encode_mesh_packet(uint8_t *buf, uint32_t to, uint32_t id,
                               uint8_t channel, uint8_t hop_limit,
                               const uint8_t *data_buf, int data_len) {
    return encode_mesh_packet(buf, 0, to, id, channel, hop_limit,
                              data_buf, data_len);
}

// ── User submessage encoding (NODEINFO_APP payload) ──
// User { id(1, string), long_name(2, string), short_name(3, string),
//        hw_model(5, varint enum), role(7, varint enum) }
// Strings are wire-type-2 (length-delimited bytes). Returns bytes written.
inline int encode_user(uint8_t *buf, const char *id, const char *long_name,
                        const char *short_name, uint32_t hw_model,
                        uint32_t role) {
    int n = 0;
    if (id && id[0]) {
        n += encode_bytes_field(buf + n, 1,
                                 (const uint8_t *)id, (int)strlen(id));
    }
    if (long_name && long_name[0]) {
        n += encode_bytes_field(buf + n, 2,
                                 (const uint8_t *)long_name,
                                 (int)strlen(long_name));
    }
    if (short_name && short_name[0]) {
        n += encode_bytes_field(buf + n, 3,
                                 (const uint8_t *)short_name,
                                 (int)strlen(short_name));
    }
    n += encode_varint_field(buf + n, 5, hw_model);
    n += encode_varint_field(buf + n, 7, role);
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

enum class ParsedKind : uint8_t { NONE = 0, TEXT, POSITION, TELEMETRY };

struct ParsedTextMessage {
    bool valid = false;
    ParsedKind kind = ParsedKind::NONE;
    uint32_t from_node = 0;
    uint32_t to_node = 0;
    uint32_t packet_id = 0;
    uint8_t channel = 0;
    uint32_t rx_time = 0;   // Unix seconds from MeshPacket.rx_time (field 9)
    char text[256] = {};
};

// ── Position / Telemetry helpers ──────────────────────
// Reinterpret a 4-byte fixed32 from the wire as a float (little-endian on ESP32,
// matching IEEE 754). Use memcpy to avoid alias-cast UB.
inline float fixed32_to_float(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Format a Position protobuf (POSITION_APP=3 payload) into a one-line summary.
// Position { latitude_i(1,sfixed32), longitude_i(2,sfixed32),
//            altitude(3,varint int32), time(4,fixed32) }
// Coords are E7 (degrees × 1e7) signed. Returns chars written (excluding NUL),
// or 0 if no usable lat/lon was found.
inline int format_position(const uint8_t *payload, int len,
                            char *out, int out_len) {
    int32_t lat_i = 0, lon_i = 0, alt = 0;
    bool have_lat = false, have_lon = false, have_alt = false;
    int pos = 0;
    while (pos < len) {
        ProtoField f;
        int consumed = decode_field(payload + pos, len - pos, &f);
        if (consumed < 0) break;
        pos += consumed;
        switch (f.field_num) {
            case 1: if (f.wire_type == 5) {
                lat_i = (int32_t)f.fixed32_val; have_lat = true;
            } break;
            case 2: if (f.wire_type == 5) {
                lon_i = (int32_t)f.fixed32_val; have_lon = true;
            } break;
            case 3: if (f.wire_type == 0) {
                alt = (int32_t)f.varint_val; have_alt = true;
            } break;
        }
    }
    if (!have_lat || !have_lon) return 0;
    double lat = (double)lat_i / 1e7;
    double lon = (double)lon_i / 1e7;
    int n;
    if (have_alt) {
        n = snprintf(out, out_len, "\xF0\x9F\x93\x8D %.4f, %.4f (alt %dm)",
                     lat, lon, (int)alt);
    } else {
        n = snprintf(out, out_len, "\xF0\x9F\x93\x8D %.4f, %.4f", lat, lon);
    }
    return (n < 0) ? 0 : n;
}

// Format a Telemetry protobuf (TELEMETRY_APP=67 payload) into a one-line summary.
// Telemetry { time(1,fixed32), device_metrics(2,bytes),
//             environment_metrics(3,bytes), ... }
// DeviceMetrics: battery_level(1,varint), voltage(2,float),
//                channel_utilization(3,float), air_util_tx(4,float)
// EnvironmentMetrics: temperature(1,float), relative_humidity(2,float),
//                     barometric_pressure(3,float)
// Returns chars written, or 0 if no recognised metrics block was found.
inline int format_telemetry(const uint8_t *payload, int len,
                             char *out, int out_len) {
    const uint8_t *dev = nullptr, *env = nullptr;
    int dev_len = 0, env_len = 0;
    int pos = 0;
    while (pos < len) {
        ProtoField f;
        int consumed = decode_field(payload + pos, len - pos, &f);
        if (consumed < 0) break;
        pos += consumed;
        if (f.wire_type == 2) {
            if (f.field_num == 2)      { dev = f.bytes_val.data; dev_len = f.bytes_val.len; }
            else if (f.field_num == 3) { env = f.bytes_val.data; env_len = f.bytes_val.len; }
        }
    }

    if (dev) {
        uint32_t battery = 0; bool have_batt = false;
        float ch_util = 0.0f; bool have_ch = false;
        int p = 0;
        while (p < dev_len) {
            ProtoField f;
            int c = decode_field(dev + p, dev_len - p, &f);
            if (c < 0) break;
            p += c;
            if (f.field_num == 1 && f.wire_type == 0) {
                battery = f.varint_val; have_batt = true;
            } else if (f.field_num == 3 && f.wire_type == 5) {
                ch_util = fixed32_to_float(f.fixed32_val); have_ch = true;
            }
        }
        if (have_batt && have_ch) {
            int n = snprintf(out, out_len, "\xF0\x9F\x94\x8B %u%%, ch %.1f%%",
                             (unsigned)battery, ch_util);
            return (n < 0) ? 0 : n;
        }
        if (have_batt) {
            int n = snprintf(out, out_len, "\xF0\x9F\x94\x8B %u%%", (unsigned)battery);
            return (n < 0) ? 0 : n;
        }
    }

    if (env) {
        float temp = 0, rh = 0, pres = 0;
        bool have_t = false, have_rh = false, have_p = false;
        int p = 0;
        while (p < env_len) {
            ProtoField f;
            int c = decode_field(env + p, env_len - p, &f);
            if (c < 0) break;
            p += c;
            if (f.wire_type != 5) continue;
            switch (f.field_num) {
                case 1: temp = fixed32_to_float(f.fixed32_val); have_t = true; break;
                case 2: rh   = fixed32_to_float(f.fixed32_val); have_rh = true; break;
                case 3: pres = fixed32_to_float(f.fixed32_val); have_p = true; break;
            }
        }
        if (have_t || have_rh || have_p) {
            char buf[128]; int bp = 0;
            bp += snprintf(buf + bp, sizeof(buf) - bp, "\xF0\x9F\x8C\xA1");
            if (have_t)  bp += snprintf(buf + bp, sizeof(buf) - bp, " %.1f\xC2\xB0""C", temp);
            if (have_rh) bp += snprintf(buf + bp, sizeof(buf) - bp, " %.0f%%RH", rh);
            if (have_p)  bp += snprintf(buf + bp, sizeof(buf) - bp, " %.0fhPa", pres);
            int n = snprintf(out, out_len, "%s", buf);
            return (n < 0) ? 0 : n;
        }
    }

    return 0;
}

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
            case 9: if (f.wire_type == 5) result.rx_time = f.fixed32_val; break;
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

    if (!payload || payload_len == 0) return result;

    if (portnum == PORTNUM_TEXT_MESSAGE_APP) {
        int copy_len = (payload_len < (int)sizeof(result.text) - 1)
                            ? payload_len : (int)sizeof(result.text) - 1;
        memcpy(result.text, payload, copy_len);
        result.text[copy_len] = '\0';
        result.kind  = ParsedKind::TEXT;
        result.valid = true;
        return result;
    }

    if (portnum == PORTNUM_POSITION_APP) {
        int n = format_position(payload, payload_len,
                                 result.text, (int)sizeof(result.text));
        if (n > 0) {
            result.kind  = ParsedKind::POSITION;
            result.valid = true;
        }
        return result;
    }

    if (portnum == PORTNUM_TELEMETRY_APP) {
        int n = format_telemetry(payload, payload_len,
                                  result.text, (int)sizeof(result.text));
        if (n > 0) {
            result.kind  = ParsedKind::TELEMETRY;
            result.valid = true;
        }
        return result;
    }

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

// ── NodeInfo parser ──────────────────────────────────
//
// FromRadio.node_info (field 4) contains:
//   NodeInfo { num: field 1 (fixed32), user: field 2 (bytes) }
//   User { long_name: field 1 (bytes), short_name: field 2 (bytes) }

struct ParsedNodeInfo {
    bool valid = false;
    uint32_t node_num = 0;
    char long_name[40] = {};
    char short_name[5] = {};
};

// Try to extract NodeInfo from a FromRadio payload.
inline ParsedNodeInfo parse_node_info(const uint8_t *buf, int len) {
    ParsedNodeInfo result;

    // FromRadio: look for field 4 (node_info)
    int pos = 0;
    const uint8_t *ni_data = nullptr;
    int ni_len = 0;

    while (pos < len) {
        ProtoField f;
        int consumed = decode_field(buf + pos, len - pos, &f);
        if (consumed < 0) break;
        pos += consumed;
        if (f.field_num == 4 && f.wire_type == 2) {
            ni_data = f.bytes_val.data;
            ni_len = f.bytes_val.len;
        }
    }

    if (!ni_data) return result;

    // NodeInfo: num(1, fixed32), user(2, bytes)
    const uint8_t *user_data = nullptr;
    int user_len = 0;
    pos = 0;

    while (pos < ni_len) {
        ProtoField f;
        int consumed = decode_field(ni_data + pos, ni_len - pos, &f);
        if (consumed < 0) break;
        pos += consumed;
        switch (f.field_num) {
            case 1: if (f.wire_type == 5) result.node_num = f.fixed32_val; break;
            case 2: if (f.wire_type == 2) { user_data = f.bytes_val.data; user_len = f.bytes_val.len; } break;
        }
    }

    if (!user_data || result.node_num == 0) return result;

    // User: long_name(1, bytes), short_name(2, bytes)
    pos = 0;
    while (pos < user_len) {
        ProtoField f;
        int consumed = decode_field(user_data + pos, user_len - pos, &f);
        if (consumed < 0) break;
        pos += consumed;
        switch (f.field_num) {
            case 1: // long_name
                if (f.wire_type == 2) {
                    int n = (f.bytes_val.len < (int)sizeof(result.long_name) - 1) ?
                             f.bytes_val.len : (int)sizeof(result.long_name) - 1;
                    memcpy(result.long_name, f.bytes_val.data, n);
                    result.long_name[n] = '\0';
                }
                break;
            case 2: // short_name
                if (f.wire_type == 2) {
                    int n = (f.bytes_val.len < (int)sizeof(result.short_name) - 1) ?
                             f.bytes_val.len : (int)sizeof(result.short_name) - 1;
                    memcpy(result.short_name, f.bytes_val.data, n);
                    result.short_name[n] = '\0';
                }
                break;
        }
    }

    result.valid = (result.long_name[0] != '\0' || result.short_name[0] != '\0');
    return result;
}

// ── Packet ID generator ──────────────────────────────

inline uint32_t generate_packet_id(uint32_t *counter) {
    uint32_t seq = ((*counter) + 1) & 0x3FF;          // lower 10 bits: sequential
    uint32_t rand_part = (esp_random() & 0x3FFFFF) << 10; // upper 22 bits: random
    *counter = seq | rand_part;
    return *counter;
}

} // namespace mesh_proto
