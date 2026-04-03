#pragma once

#include "noise_state.h"
#include "../config.h"

// ── Noise XX per-peer handshake state machine ─────────────────────────
//
// Usage (initiator):
//   noise_hs_init(&hs, NOISE_ROLE_INITIATOR, s_priv, s_pub);
//   noise_hs_write_msg1(&hs, buf, &len);   // send PKT_NOISE_HANDSHAKE
//   // ... receive peer's msg2 ...
//   noise_hs_read_msg2_write_msg3(&hs, msg2, msg2_len, buf, &len); // send msg3
//   // hs.phase == NOISE_HS_TRANSPORT → ready
//
// Usage (responder):
//   noise_hs_init(&hs, NOISE_ROLE_RESPONDER, s_priv, s_pub);
//   // ... receive peer's msg1 ...
//   noise_hs_read_msg1_write_msg2(&hs, msg1, msg1_len, buf, &len); // send msg2
//   // ... receive peer's msg3 ...
//   noise_hs_read_msg3(&hs, msg3, msg3_len);
//   // hs.phase == NOISE_HS_TRANSPORT → ready

typedef enum {
    NOISE_HS_IDLE       = 0,
    NOISE_HS_AWAIT_MSG2,     // initiator sent msg1, waiting for msg2
    NOISE_HS_AWAIT_MSG3,     // responder sent msg2, waiting for msg3
    NOISE_HS_TRANSPORT,      // handshake complete, transport keys active
    NOISE_HS_FAILED
} NoiseHSPhase;

typedef enum {
    NOISE_ROLE_INITIATOR = 0,
    NOISE_ROLE_RESPONDER
} NoiseRole;

struct NoiseHandshakeState {
    NoiseRole           role;
    NoiseHSPhase        phase;
    NoiseSymmetricState ss;

    // Local ephemeral keypair (generated fresh per handshake)
    uint8_t e_priv[32];
    uint8_t e_pub[32];

    // Remote keys learned during handshake
    uint8_t re_pub[32];      // remote ephemeral
    uint8_t rs_pub[32];      // remote static

    // Pointers to our persistent static keypair
    const uint8_t *s_priv;
    const uint8_t *s_pub;

    // Transport cipher states (valid after NOISE_HS_TRANSPORT)
    NoiseCipherState send_cs;
    NoiseCipherState recv_cs;
};

// ── API ───────────────────────────────────────────────────────────────

// Initialise a fresh handshake state and generate an ephemeral keypair.
int noise_hs_init(NoiseHandshakeState *hs, NoiseRole role,
                  const uint8_t *s_priv, const uint8_t *s_pub);

// Initiator → Responder: build message 1 (raw bytes).
// out receives e_pub[32]. These go directly into the PKT_NOISE_HANDSHAKE payload.
int noise_hs_write_msg1(NoiseHandshakeState *hs, uint8_t *out, size_t *out_len);

// Responder: consume message 1 (32B), produce message 2 (80B raw).
// msg2 = e_pub[32] + EncryptAndHash(s_pub)[32+16]
int noise_hs_read_msg1_write_msg2(NoiseHandshakeState *hs,
                                   const uint8_t *msg1, size_t msg1_len,
                                   uint8_t *out, size_t *out_len);

// Initiator: consume message 2 (80B), produce message 3 (48B raw).
// msg3 = EncryptAndHash(s_pub)[32+16]
int noise_hs_read_msg2_write_msg3(NoiseHandshakeState *hs,
                                   const uint8_t *msg2, size_t msg2_len,
                                   uint8_t *out, size_t *out_len);

// Responder: consume message 3 (48B) and finalise.
// On success, hs->phase == NOISE_HS_TRANSPORT.
int noise_hs_read_msg3(NoiseHandshakeState *hs,
                        const uint8_t *msg3, size_t msg3_len);

// Transport encrypt (after NOISE_HS_TRANSPORT).
// ad: additional data (use the 14-byte bitchat packet header).
// ct receives ciphertext; *ct_len = plen + 16.
int noise_hs_encrypt(NoiseHandshakeState *hs,
                      const uint8_t *ad, size_t ad_len,
                      const uint8_t *pt, size_t plen,
                      uint8_t *ct, size_t *ct_len);

// Transport decrypt.
int noise_hs_decrypt(NoiseHandshakeState *hs,
                      const uint8_t *ad, size_t ad_len,
                      const uint8_t *ct, size_t ct_len,
                      uint8_t *pt, size_t *pt_len);
