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
