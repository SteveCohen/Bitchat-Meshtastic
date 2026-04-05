#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "noise_state.h"
#include "ed25519.h"
#include <nvs_flash.h>
#include <nvs.h>

// ── Bitchat identity ─────────────────────────────────────────────────
//
// Each bitchat node has a persistent keypair:
//   noise_private[32] / noise_public[32]  — Curve25519 for Noise XX
//   sign_private[64] / sign_public[32]     — Ed25519 for packet signing
//   fingerprint[32]                        — SHA-256(noise_public)
//   peer_id = fingerprint[0..7]            — 8-byte ID in packet headers
//
// Keys are persisted in NVS under namespace "bc", key "kp".

struct BitchatKeypair {
    uint8_t noise_private[32];  // Curve25519 private key (little-endian)
    uint8_t noise_public[32];   // Curve25519 public key  (little-endian)
    uint8_t sign_private[64];   // Ed25519 private key (seed || public, NaCl format)
    uint8_t sign_public[32];    // Ed25519 public key (compressed Edwards point)
    uint8_t fingerprint[32];    // SHA-256(noise_public); peer_id = first 8 bytes
};

// Generate or load the bridge's bitchat identity.
// Returns true on success. Safe to call multiple times (idempotent NVS init).
inline bool bitchat_identity_init(BitchatKeypair *kp) {
    // Ensure NVS is initialised (safe to call multiple times)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        // NVS unavailable — generate ephemeral keys (lost on reboot)
        goto generate;
    }

    {
        nvs_handle_t h;
        err = nvs_open("bc", NVS_READWRITE, &h);
        if (err == ESP_OK) {
            size_t sz = sizeof(BitchatKeypair);
            esp_err_t lerr = nvs_get_blob(h, "kp", kp, &sz);
            nvs_close(h);
            if (lerr == ESP_OK && sz == sizeof(BitchatKeypair)) {
                // Loaded successfully
                return true;
            }
        }
    }

generate:
    // Generate a fresh Curve25519 keypair
    memset(kp, 0, sizeof(BitchatKeypair));
    if (noise_gen_keypair(kp->noise_private, kp->noise_public) != 0) {
        return false;
    }

    // Generate Ed25519 signing keypair from random seed
    {
        uint8_t seed[32];
        esp_fill_random(seed, 32);
        ed25519_create_keypair(seed, kp->sign_public, kp->sign_private);
        memset(seed, 0, sizeof(seed));
    }

    // fingerprint = SHA-256(noise_public)
    if (noise_sha256(kp->noise_public, 32, kp->fingerprint) != 0) {
        return false;
    }

    // Persist to NVS
    {
        nvs_handle_t h;
        if (nvs_open("bc", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "kp", kp, sizeof(BitchatKeypair));
            nvs_commit(h);
            nvs_close(h);
        }
    }

    return true;
}

// Format the first 4 bytes of fingerprint as hex (8 chars + NUL).
inline void bitchat_fingerprint_short(const uint8_t fp[32], char *out, int out_len) {
    snprintf(out, out_len, "%02x%02x%02x%02x", fp[0], fp[1], fp[2], fp[3]);
}

// ── Virtual identity derivation ─────────────────────────────────────
//
// Derive a deterministic virtual keypair from the bridge master key and
// a Meshtastic node ID. The same (master, node_id) always produces the
// same output. No NVS storage needed.
//
// HKDF-SHA256:  salt = "bitchat-virtual-id"
//               IKM  = master->noise_private
//               info = node_id (4 bytes, little-endian) + purpose byte
//
// Purpose 0x01 → 32 bytes → Curve25519 private key (clamped)
// Purpose 0x02 → 32 bytes → Ed25519 seed → derive sign keypair

inline bool bitchat_derive_keypair(BitchatKeypair *out,
                                    const BitchatKeypair *master,
                                    uint32_t node_id) {
    memset(out, 0, sizeof(BitchatKeypair));

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    const uint8_t salt[] = "bitchat-virtual-id";

    // Info = node_id (LE) + purpose byte
    uint8_t info[5];
    info[0] = (node_id      ) & 0xFF;
    info[1] = (node_id >>  8) & 0xFF;
    info[2] = (node_id >> 16) & 0xFF;
    info[3] = (node_id >> 24) & 0xFF;

    // Purpose 0x01: derive Curve25519 private key
    info[4] = 0x01;
    uint8_t noise_seed[32];
    if (mbedtls_hkdf(md, salt, sizeof(salt) - 1,
                     master->noise_private, 32,
                     info, 5,
                     noise_seed, 32) != 0) return false;

    // Clamp for Curve25519 (RFC 7748)
    noise_seed[0]  &= 248;
    noise_seed[31] &= 127;
    noise_seed[31] |= 64;
    memcpy(out->noise_private, noise_seed, 32);

    // Derive Curve25519 public key: pub = scalar_mult(private, base_point)
    // Uses the same approach as noise_gen_keypair but with our derived private key
    {
        mbedtls_ecp_group grp;
        mbedtls_mpi d, z;
        mbedtls_ecp_group_init(&grp);
        mbedtls_mpi_init(&d);
        mbedtls_mpi_init(&z);

        int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
        if (ret == 0) ret = mbedtls_mpi_read_binary_le(&d, out->noise_private, 32);
        // ecdh_compute_shared(grp, z, Q, d, ...) computes z = X(d * Q)
        // With Q = G (generator), this gives us the public key X-coordinate
        if (ret == 0) ret = mbedtls_ecdh_compute_shared(&grp, &z,
                                &grp.G, &d, nullptr, nullptr);
        if (ret == 0) ret = mbedtls_mpi_write_binary_le(&z, out->noise_public, 32);

        mbedtls_mpi_free(&z);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_group_free(&grp);
        if (ret != 0) return false;
    }

    // Purpose 0x02: derive Ed25519 signing keypair from seed
    info[4] = 0x02;
    uint8_t sign_seed[32];
    if (mbedtls_hkdf(md, salt, sizeof(salt) - 1,
                     master->noise_private, 32,
                     info, 5,
                     sign_seed, 32) != 0) return false;

    ed25519_create_keypair(sign_seed, out->sign_public, out->sign_private);
    memset(sign_seed, 0, sizeof(sign_seed));

    // Fingerprint = SHA-256(noise_public)
    if (noise_sha256(out->noise_public, 32, out->fingerprint) != 0) return false;

    memset(noise_seed, 0, sizeof(noise_seed));
    return true;
}
