#pragma once

// ── Noise_XX_25519_ChaChaPoly_SHA256 — low-level primitives ──────────
//
// This header-only unit implements the symmetric-state layer of the
// Noise protocol framework, plus Curve25519 DH helpers, using the
// mbedtls library bundled with ESP-IDF. No additional PlatformIO
// dependencies are required.
//
// Reference: https://noiseprotocol.org/noise.html

#include <cstdint>
#include <cstring>
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <esp_random.h>

// mbedtls 3.x marks struct fields as private and requires either
// MBEDTLS_ALLOW_PRIVATE_ACCESS or the MBEDTLS_PRIVATE() accessor.
// On older mbedtls 2.x (shipped with some arduino-esp32 versions) the
// macro doesn't exist and fields are plain members, so provide a shim.
#ifndef MBEDTLS_PRIVATE
#define MBEDTLS_PRIVATE(member) member
#endif

// ── Noise protocol name (exactly 32 bytes) ───────────────────────────
static const char NOISE_PROTO_NAME[32] = "Noise_XX_25519_ChaChaPoly_SHA256";

// ── Data structures ──────────────────────────────────────────────────

struct NoiseSymmetricState {
    uint8_t  ck[32];     // chaining key
    uint8_t  h[32];      // running hash
    uint8_t  k[32];      // current AEAD key (valid when has_key)
    uint64_t n;          // nonce counter
    bool     has_key;
};

struct NoiseCipherState {
    uint8_t  k[32];
    uint64_t n;
};

// ── RNG wrapper for mbedtls callbacks ────────────────────────────────

static inline int _noise_rng(void * /*ctx*/, unsigned char *buf, size_t len) {
    esp_fill_random(buf, len);
    return 0;
}

// ── Nonce helper: 4 zero bytes + 8-byte little-endian counter ────────

static inline void _noise_nonce(uint8_t nonce[12], uint64_t n) {
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (n >> (i * 8)) & 0xFF;
}

// ── SHA-256 helpers ───────────────────────────────────────────────────

// One-shot SHA-256
static inline int _sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int ret = mbedtls_sha256_starts(&ctx, 0);
    if (ret == 0) ret = mbedtls_sha256_update(&ctx, data, len);
    if (ret == 0) ret = mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    return ret;
}

// ── Symmetric state primitives ────────────────────────────────────────

// Initialize SymmetricState: ck = h = NOISE_PROTO_NAME (exactly 32 bytes),
// then MixHash(prologue). The iOS app always calls MixHash(prologue) even
// with empty prologue, which changes h from raw protocol name to SHA256(h).
static inline int noise_ss_init(NoiseSymmetricState *ss) {
    memcpy(ss->ck, NOISE_PROTO_NAME, 32);
    memcpy(ss->h,  NOISE_PROTO_NAME, 32);
    memset(ss->k, 0, 32);
    ss->n       = 0;
    ss->has_key = false;
    // MixHash(empty prologue) — matches iOS app's mixPreMessageKeys()
    noise_mix_hash(ss, (const uint8_t *)"", 0);
    return 0;
}

// MixHash(data): h = SHA256(h || data)
static inline void noise_mix_hash(NoiseSymmetricState *ss,
                                   const uint8_t *data, size_t len) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, ss->h, 32);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, ss->h);
    mbedtls_sha256_free(&ctx);
}

// MixKey(ikm): (ck, k) = HKDF(ck, ikm); reset n = 0
static inline int noise_mix_key(NoiseSymmetricState *ss,
                                  const uint8_t *ikm, size_t ikm_len) {
    uint8_t out[64];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    // HKDF: salt=ck, ikm=ikm, info="" → 64 bytes of output
    int ret = mbedtls_hkdf(md,
                            ss->ck, 32,          // salt
                            ikm,    ikm_len,      // IKM
                            nullptr, 0,           // info (empty)
                            out,    sizeof(out)); // OKM: 64 bytes
    if (ret == 0) {
        memcpy(ss->ck, out,      32);  // output1 → new ck
        memcpy(ss->k,  out + 32, 32);  // output2 → new k
        ss->n       = 0;
        ss->has_key = true;
    }
    memset(out, 0, sizeof(out));
    return ret;
}

// EncryptAndHash(plaintext) → ciphertext (plen + 16 tag) + MixHash(ct)
// If no key yet, copies plaintext to ciphertext and MixHashes it.
static inline int noise_encrypt_and_hash(NoiseSymmetricState *ss,
                                          const uint8_t *pt, size_t plen,
                                          uint8_t *ct, size_t *ct_len) {
    if (!ss->has_key) {
        // No key: pass through, just MixHash
        memcpy(ct, pt, plen);
        *ct_len = plen;
        noise_mix_hash(ss, ct, plen);
        return 0;
    }

    uint8_t nonce[12];
    _noise_nonce(nonce, ss->n++);

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    int ret = mbedtls_chachapoly_setkey(&ctx, ss->k);
    if (ret == 0) {
        ret = mbedtls_chachapoly_encrypt_and_tag(
            &ctx, plen, nonce,
            ss->h, 32,          // AAD = current h
            pt,    ct,          // plaintext → ciphertext
            ct + plen);         // 16-byte tag appended
    }
    mbedtls_chachapoly_free(&ctx);

    if (ret == 0) {
        *ct_len = plen + 16;
        noise_mix_hash(ss, ct, *ct_len);
    }
    return ret;
}

// DecryptAndHash(ciphertext) → plaintext + MixHash(ct)
// If no key yet, copies ciphertext to plaintext and MixHashes it.
static inline int noise_decrypt_and_hash(NoiseSymmetricState *ss,
                                          const uint8_t *ct, size_t ct_len,
                                          uint8_t *pt, size_t *pt_len) {
    if (!ss->has_key) {
        if (ct_len == 0) { *pt_len = 0; return 0; }
        memcpy(pt, ct, ct_len);
        *pt_len = ct_len;
        noise_mix_hash(ss, ct, ct_len);
        return 0;
    }

    if (ct_len < 16) return -1;  // too short to have a tag
    size_t plen = ct_len - 16;

    uint8_t nonce[12];
    _noise_nonce(nonce, ss->n++);

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    int ret = mbedtls_chachapoly_setkey(&ctx, ss->k);
    if (ret == 0) {
        ret = mbedtls_chachapoly_auth_decrypt(
            &ctx, plen, nonce,
            ss->h, 32,                  // AAD = current h
            ct + plen,                  // 16-byte tag
            ct,    pt);                 // ciphertext → plaintext
    }
    mbedtls_chachapoly_free(&ctx);

    if (ret == 0) {
        *pt_len = plen;
        noise_mix_hash(ss, ct, ct_len);
    }
    return ret;
}

// Split() → (send_cs, recv_cs). Call after handshake is complete.
static inline int noise_split(const NoiseSymmetricState *ss,
                               NoiseCipherState *send_cs,
                               NoiseCipherState *recv_cs) {
    uint8_t out[64];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    // HKDF: salt=ck, ikm="" (zero-length), info="" → 64 bytes
    uint8_t empty = 0;
    int ret = mbedtls_hkdf(md,
                            ss->ck, 32,
                            &empty, 0,  // zero-length IKM
                            nullptr, 0,
                            out, 64);
    if (ret == 0) {
        memcpy(send_cs->k, out,      32);
        memcpy(recv_cs->k, out + 32, 32);
        send_cs->n = 0;
        recv_cs->n = 0;
    }
    memset(out, 0, sizeof(out));
    return ret;
}

// Transport encrypt: c = AEAD(k, n++, ad, plaintext)
static inline int noise_cs_encrypt(NoiseCipherState *cs,
                                    const uint8_t *ad, size_t ad_len,
                                    const uint8_t *pt, size_t plen,
                                    uint8_t *ct, size_t *ct_len) {
    uint8_t nonce[12];
    _noise_nonce(nonce, cs->n++);

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    int ret = mbedtls_chachapoly_setkey(&ctx, cs->k);
    if (ret == 0) {
        ret = mbedtls_chachapoly_encrypt_and_tag(
            &ctx, plen, nonce,
            ad, ad_len,
            pt, ct, ct + plen);
    }
    mbedtls_chachapoly_free(&ctx);
    if (ret == 0) *ct_len = plen + 16;
    return ret;
}

// Transport decrypt: p = AEAD_decrypt(k, n++, ad, ciphertext)
static inline int noise_cs_decrypt(NoiseCipherState *cs,
                                    const uint8_t *ad, size_t ad_len,
                                    const uint8_t *ct, size_t ct_len,
                                    uint8_t *pt, size_t *pt_len) {
    if (ct_len < 16) return -1;
    size_t plen = ct_len - 16;

    uint8_t nonce[12];
    _noise_nonce(nonce, cs->n++);

    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    int ret = mbedtls_chachapoly_setkey(&ctx, cs->k);
    if (ret == 0) {
        ret = mbedtls_chachapoly_auth_decrypt(
            &ctx, plen, nonce,
            ad, ad_len,
            ct + plen,   // tag
            ct, pt);
    }
    mbedtls_chachapoly_free(&ctx);
    if (ret == 0) *pt_len = plen;
    return ret;
}

// ── Curve25519 helpers ────────────────────────────────────────────────
//
// mbedtls ECP stores coordinates as big-endian MPIs. Curve25519 wire
// format (RFC 7748) is little-endian. We use mbedtls_mpi_write_binary_le
// / read_binary_le throughout.

// Generate a Curve25519 keypair. priv[32] and pub[32] are little-endian.
static inline int noise_gen_keypair(uint8_t priv[32], uint8_t pub[32]) {
    mbedtls_ecp_group grp;
    mbedtls_mpi      d;
    mbedtls_ecp_point Q;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (ret == 0) ret = mbedtls_ecdh_gen_public(&grp, &d, &Q, _noise_rng, nullptr);
    if (ret == 0) ret = mbedtls_mpi_write_binary_le(&d, priv, 32);
    if (ret == 0) ret = mbedtls_mpi_write_binary_le(&Q.MBEDTLS_PRIVATE(X), pub, 32);

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ret;
}

// Compute X25519 shared secret. All keys are 32-byte little-endian.
static inline int noise_dh(const uint8_t priv[32], const uint8_t remote_pub[32],
                             uint8_t shared[32]) {
    mbedtls_ecp_group grp;
    mbedtls_mpi      d, z;
    mbedtls_ecp_point Q;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&Q);

    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (ret == 0) ret = mbedtls_mpi_read_binary_le(&d, priv, 32);
    if (ret == 0) ret = mbedtls_mpi_read_binary_le(&Q.MBEDTLS_PRIVATE(X), remote_pub, 32);
    if (ret == 0) ret = mbedtls_mpi_lset(&Q.MBEDTLS_PRIVATE(Z), 1); // affine
    if (ret == 0) ret = mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d, _noise_rng, nullptr);
    if (ret == 0) ret = mbedtls_mpi_write_binary_le(&z, shared, 32);

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);

    return ret;
}

// Compute SHA-256 of data, write into out[32]. Convenience wrapper.
static inline int noise_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    return _sha256(data, len, out);
}
