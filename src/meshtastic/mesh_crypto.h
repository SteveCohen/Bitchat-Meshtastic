#pragma once

// ── Meshtastic channel-key crypto (AES-CTR) ─────────────────────────
//
// Used by the UDP-multicast transport (and any other path that emits
// encrypted MeshPackets) to encrypt/decrypt the SubPacket bytes that
// go into MeshPacket.encrypted (field 22).
//
// IV layout per upstream firmware src/mesh/CryptoEngine.cpp::initNonce:
//   bytes 0..7  = packet_id  (uint64_t little-endian; high 32 bits zero
//                              because protobuf id is uint32)
//   bytes 8..11 = from_node  (uint32_t little-endian)
//   bytes 12..15 = extra_nonce (uint32_t LE; 0 for channel-key path)
//
// Key length:
//   16 bytes -> AES-128 (default LongFast channel)
//   32 bytes -> AES-256 (custom 32-byte PSK)

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mbedtls/aes.h>

// 16-byte default PSK for the public LongFast channel, copied verbatim
// from upstream src/mesh/Channels.h:
//   static const uint8_t defaultpsk[] = { 0xd4, 0xf1, 0xbb, 0x3a, ... };
static const uint8_t MESH_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

static inline void _mesh_build_iv(uint8_t iv[16],
                                  uint32_t packet_id,
                                  uint32_t from_node) {
    memset(iv, 0, 16);
    // packet_id at bytes 0..3 (uint64 LE; high 4 bytes stay zero)
    iv[0] = (uint8_t)(packet_id      );
    iv[1] = (uint8_t)(packet_id >>  8);
    iv[2] = (uint8_t)(packet_id >> 16);
    iv[3] = (uint8_t)(packet_id >> 24);
    // from_node at bytes 8..11 (uint32 LE)
    iv[8]  = (uint8_t)(from_node      );
    iv[9]  = (uint8_t)(from_node >>  8);
    iv[10] = (uint8_t)(from_node >> 16);
    iv[11] = (uint8_t)(from_node >> 24);
    // bytes 12..15 left zero (extra_nonce only used for PKI/Curve25519 path)
}

// AES-CTR is symmetric: same call encrypts and decrypts.
// Returns 0 on success, non-zero mbedtls error on failure.
// `key_len` is bytes (16 or 32).
static inline int mesh_crypt(uint32_t packet_id,
                              uint32_t from_node,
                              const uint8_t *key, size_t key_len,
                              const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t iv[16];
    _mesh_build_iv(iv, packet_id, from_node);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_enc(&ctx, key, (unsigned int)(key_len * 8));
    if (ret != 0) {
        mbedtls_aes_free(&ctx);
        return ret;
    }

    size_t nc_off = 0;
    uint8_t stream_block[16] = {};
    ret = mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, iv, stream_block, in, out);
    mbedtls_aes_free(&ctx);
    return ret;
}
