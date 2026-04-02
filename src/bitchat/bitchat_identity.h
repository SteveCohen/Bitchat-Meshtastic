#pragma once

#include <cstdint>

// ── Bitchat Identity Stubs ───────────────────────────────────────────
//
// Bitchat uses two key pairs per user:
//   1. Curve25519 (for Noise_XX handshake — encryption)
//   2. Ed25519   (for message signing — authentication)
//
// The user's fingerprint = SHA-256(noise_static_public_key)
// The peer ID = first 8 bytes of the fingerprint (used in packet headers)
// Broadcast ID = 0xFFFFFFFFFFFFFFFF
//
// Phase 1: The bridge uses a single keypair for all bridged messages.
// Phase 2: The bridge maintains per-Meshtastic-user virtual identities.

struct BitchatKeypair {
    uint8_t noise_private[32];   // Curve25519 private key
    uint8_t noise_public[32];    // Curve25519 public key
    uint8_t sign_private[64];    // Ed25519 private key (64 bytes: seed + public)
    uint8_t sign_public[32];     // Ed25519 public key
    uint8_t fingerprint[32];     // SHA-256(noise_public) — first 8 bytes = peer ID
};

// Generate or load the bridge's own bitchat identity.
// Phase 1: generates a random keypair on first boot and stores in NVS.
// Returns true on success.
inline bool bitchat_identity_init(BitchatKeypair *kp) {
    // TODO: implement actual key generation using:
    //   - mbedtls_ecp_gen_keypair() for Curve25519
    //   - mbedtls_pk_generate_key() or sodium for Ed25519
    //   - mbedtls_sha256() for fingerprint
    //   - NVS for persistence
    //
    // For now, fill with deterministic placeholder so the bridge can run
    memset(kp, 0, sizeof(BitchatKeypair));
    // Mark as placeholder
    kp->fingerprint[0] = 0xBB; // "Bridge Bitchat"
    return true;
}

// Format a fingerprint as a short hex string (first 4 bytes).
inline void bitchat_fingerprint_short(const uint8_t fp[32], char *out, int out_len) {
    snprintf(out, out_len, "%02x%02x%02x%02x", fp[0], fp[1], fp[2], fp[3]);
}
