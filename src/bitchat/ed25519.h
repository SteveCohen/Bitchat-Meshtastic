#pragma once

// ── Compact Ed25519 (sign + keygen) for ESP32 ───────────────────────
//
// Based on the tweetnacl algorithms by Daniel J. Bernstein et al.
// (public domain). Uses mbedtls SHA-512 from ESP-IDF for hashing.
//
// Provides: ed25519_create_keypair(), ed25519_sign()

#include <cstdint>
#include <cstring>
#include <mbedtls/sha512.h>

// ── Field element: GF(2^255-19), 16 limbs of ~16 bits ───────────────

typedef int64_t ed_gf[16];

static const ed_gf _ed_gf0 = {0};
static const ed_gf _ed_gf1 = {1};

static const ed_gf _ed_D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203
};
static const ed_gf _ed_D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406
};
static const ed_gf _ed_I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83
};

// Group order L
static const uint8_t _ed_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10
};

// Ed25519 base point (compressed encoding)
static const uint8_t _ed_B[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66
};

// ── Field arithmetic ─────────────────────────────────────────────────

static inline void _ed_set(ed_gf o, const ed_gf a) {
    for (int i = 0; i < 16; i++) o[i] = a[i];
}

static inline void _ed_car(ed_gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static inline void _ed_sel(ed_gf p, ed_gf q, int b) {
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static inline void _ed_pack25519(uint8_t *o, const ed_gf n) {
    ed_gf m, t;
    _ed_set(t, n);
    _ed_car(t); _ed_car(t); _ed_car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int bv = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        _ed_sel(t, m, 1 - bv);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i]     = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static inline void _ed_unpack25519(ed_gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++)
        o[i] = (int64_t)n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static inline int _ed_par25519(const ed_gf a) {
    uint8_t d[32];
    _ed_pack25519(d, a);
    return d[0] & 1;
}

static inline int _ed_neq25519(const ed_gf a, const ed_gf b) {
    uint8_t c[32], d[32];
    _ed_pack25519(c, a);
    _ed_pack25519(d, b);
    return memcmp(c, d, 32) != 0;
}

static inline void _ed_A(ed_gf o, const ed_gf a, const ed_gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static inline void _ed_Z(ed_gf o, const ed_gf a, const ed_gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static inline void _ed_M(ed_gf o, const ed_gf a, const ed_gf b) {
    int64_t t[31] = {};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    _ed_car(o); _ed_car(o);
}

static inline void _ed_S(ed_gf o, const ed_gf a) { _ed_M(o, a, a); }

static inline void _ed_inv25519(ed_gf o, const ed_gf a) {
    ed_gf c;
    _ed_set(c, a);
    for (int i = 253; i >= 0; i--) {
        _ed_S(c, c);
        if (i != 2 && i != 4) _ed_M(c, c, a);
    }
    _ed_set(o, c);
}

static inline void _ed_pow2523(ed_gf o, const ed_gf a) {
    ed_gf c;
    _ed_set(c, a);
    for (int i = 250; i >= 0; i--) {
        _ed_S(c, c);
        if (i != 1) _ed_M(c, c, a);
    }
    _ed_set(o, c);
}

// ── Point operations (extended coordinates [X:Y:Z:T]) ────────────────

static inline void _ed_cswap(ed_gf p[4], ed_gf q[4], uint8_t b) {
    for (int i = 0; i < 4; i++) _ed_sel(p[i], q[i], (int)b);
}

static inline void _ed_pt_add(ed_gf p[4], ed_gf q[4]) {
    ed_gf a, b, c, d, t, e, f, g, h;
    _ed_Z(a, p[1], p[0]);
    _ed_Z(t, q[1], q[0]);
    _ed_M(a, a, t);
    _ed_A(b, p[0], p[1]);
    _ed_A(t, q[0], q[1]);
    _ed_M(b, b, t);
    _ed_M(c, p[3], q[3]);
    _ed_M(c, c, _ed_D2);
    _ed_M(d, p[2], q[2]);
    _ed_A(d, d, d);
    _ed_Z(e, b, a);
    _ed_Z(f, d, c);
    _ed_A(g, d, c);
    _ed_A(h, b, a);
    _ed_M(p[0], e, f);
    _ed_M(p[1], h, g);
    _ed_M(p[2], g, f);
    _ed_M(p[3], e, h);
}

static inline void _ed_pack_point(uint8_t *r, ed_gf p[4]) {
    ed_gf tx, ty, zi;
    _ed_inv25519(zi, p[2]);
    _ed_M(tx, p[0], zi);
    _ed_M(ty, p[1], zi);
    _ed_pack25519(r, ty);
    r[31] ^= (uint8_t)(_ed_par25519(tx) << 7);
}

static inline int _ed_unpackneg(ed_gf r[4], const uint8_t p[32]) {
    ed_gf t, chk, num, den, den2, den4, den6;
    _ed_set(r[2], _ed_gf1);
    _ed_unpack25519(r[1], p);
    _ed_S(num, r[1]);           // num = y^2
    _ed_M(den, num, _ed_D);    // den = d*y^2
    _ed_Z(num, num, r[2]);     // num = y^2 - 1
    _ed_A(den, r[2], den);     // den = 1 + d*y^2

    _ed_S(den2, den);
    _ed_S(den4, den2);
    _ed_M(den6, den4, den2);
    _ed_M(t, den6, num);
    _ed_M(t, t, den);

    _ed_pow2523(t, t);
    _ed_M(t, t, num);
    _ed_M(t, t, den);
    _ed_M(t, t, den);
    _ed_M(r[0], t, den);

    _ed_S(chk, r[0]);
    _ed_M(chk, chk, den);
    if (_ed_neq25519(chk, num)) _ed_M(r[0], r[0], _ed_I);

    _ed_S(chk, r[0]);
    _ed_M(chk, chk, den);
    if (_ed_neq25519(chk, num)) return -1;

    if (_ed_par25519(r[0]) == (p[31] >> 7)) _ed_Z(r[0], _ed_gf0, r[0]);

    _ed_M(r[3], r[0], r[1]);
    return 0;
}

static inline void _ed_scalarmult(ed_gf p[4], ed_gf q[4], const uint8_t *s) {
    _ed_set(p[0], _ed_gf0);
    _ed_set(p[1], _ed_gf1);
    _ed_set(p[2], _ed_gf1);
    _ed_set(p[3], _ed_gf0);
    for (int i = 255; i >= 0; i--) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        _ed_cswap(p, q, b);
        _ed_pt_add(q, p);
        _ed_pt_add(p, p);
        _ed_cswap(p, q, b);
    }
}

static inline void _ed_scalarbase(ed_gf p[4], const uint8_t *s) {
    ed_gf q[4];
    _ed_unpackneg(q, _ed_B);
    // unpackneg gives -B, negate x and t to get +B
    _ed_Z(q[0], _ed_gf0, q[0]);
    _ed_Z(q[3], _ed_gf0, q[3]);
    _ed_scalarmult(p, q, s);
}

// ── Scalar reduction mod L ───────────────────────────────────────────

static inline void _ed_reduce(uint8_t *r, int64_t x[64]) {
    for (int i = 63; i >= 32; i--) {
        int64_t carry = 0;
        for (int j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * (int64_t)_ed_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[i - 12] += carry;
        x[i] = 0;
    }
    int64_t carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * (int64_t)_ed_L[j];
        carry = x[j] >> 8;
        x[j] &= 0xff;
    }
    for (int j = 0; j < 32; j++)
        x[j] -= carry * (int64_t)_ed_L[j];
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 0xff);
    }
}

// ── Public API ───────────────────────────────────────────────────────

// Create Ed25519 keypair from 32-byte seed.
// sk[64] = seed || public_key (standard NaCl format)
static inline void ed25519_create_keypair(const uint8_t seed[32],
                                           uint8_t pk[32],
                                           uint8_t sk[64]) {
    uint8_t d[64];
    mbedtls_sha512(seed, 32, d, 0);
    d[0]  &= 248;
    d[31] &= 127;
    d[31] |= 64;

    ed_gf p[4];
    _ed_scalarbase(p, d);
    _ed_pack_point(pk, p);

    memcpy(sk, seed, 32);
    memcpy(sk + 32, pk, 32);

    memset(d, 0, sizeof(d));
}

// Sign message. Writes 64-byte signature.
static inline void ed25519_sign(uint8_t sig[64],
                                 const uint8_t *msg, size_t msg_len,
                                 const uint8_t sk[64]) {
    uint8_t d[64], h[64];
    mbedtls_sha512(sk, 32, d, 0);  // expand seed
    d[0]  &= 248;
    d[31] &= 127;
    d[31] |= 64;

    // r = SHA-512(d[32..63] || msg) — deterministic nonce
    {
        mbedtls_sha512_context ctx;
        mbedtls_sha512_init(&ctx);
        mbedtls_sha512_starts(&ctx, 0);
        mbedtls_sha512_update(&ctx, d + 32, 32);
        mbedtls_sha512_update(&ctx, msg, msg_len);
        mbedtls_sha512_finish(&ctx, h);
        mbedtls_sha512_free(&ctx);
    }

    // Reduce r mod L
    int64_t x[64] = {};
    for (int i = 0; i < 64; i++) x[i] = (int64_t)(uint64_t)h[i];
    _ed_reduce(sig + 32, x);  // sig[32..63] = r mod L

    // R = r * B
    ed_gf p[4];
    _ed_scalarbase(p, sig + 32);
    _ed_pack_point(sig, p);  // sig[0..31] = R

    // hram = SHA-512(R || pk || msg)
    {
        mbedtls_sha512_context ctx;
        mbedtls_sha512_init(&ctx);
        mbedtls_sha512_starts(&ctx, 0);
        mbedtls_sha512_update(&ctx, sig, 32);        // R
        mbedtls_sha512_update(&ctx, sk + 32, 32);    // pk
        mbedtls_sha512_update(&ctx, msg, msg_len);
        mbedtls_sha512_finish(&ctx, h);
        mbedtls_sha512_free(&ctx);
    }

    // S = (r + hram * a) mod L
    memset(x, 0, sizeof(x));
    for (int i = 0; i < 32; i++) x[i] = (int64_t)(uint64_t)sig[32 + i];  // r
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++)
            x[i + j] += (int64_t)(uint64_t)h[i] * (int64_t)(uint64_t)d[j];
    _ed_reduce(sig + 32, x);  // sig[32..63] = S

    memset(d, 0, sizeof(d));
}
