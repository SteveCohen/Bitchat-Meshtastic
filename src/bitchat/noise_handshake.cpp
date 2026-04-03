#include "noise_handshake.h"
#include "../config.h"
#include <Arduino.h>
#include <cstring>

static const char *TAG = "Noise";

// ── Public API ────────────────────────────────────────────────────────

int noise_hs_init(NoiseHandshakeState *hs, NoiseRole role,
                  const uint8_t *s_priv, const uint8_t *s_pub) {
    memset(hs, 0, sizeof(*hs));
    hs->role   = role;
    hs->phase  = NOISE_HS_IDLE;
    hs->s_priv = s_priv;
    hs->s_pub  = s_pub;

    int ret = noise_ss_init(&hs->ss);
    if (ret != 0) return ret;

    return noise_gen_keypair(hs->e_priv, hs->e_pub);
}

// ── Message 1: Initiator → Responder ─────────────────────────────────
// Pattern tokens: e
// Wire format: e_pub[32]  (raw bytes, no TLV)

int noise_hs_write_msg1(NoiseHandshakeState *hs, uint8_t *out, size_t *out_len) {
    if (hs->phase != NOISE_HS_IDLE || hs->role != NOISE_ROLE_INITIATOR) return -1;

    // Send: e
    memcpy(out, hs->e_pub, 32);
    *out_len = 32;

    // MixHash(e)
    noise_mix_hash(&hs->ss, hs->e_pub, 32);

    hs->phase = NOISE_HS_AWAIT_MSG2;
    Serial.printf("[%s] Sent msg1 (e, 32B)\n", TAG);
    return 0;
}

// ── Message 2: Responder → Initiator ─────────────────────────────────
// Pattern tokens: e, ee, s, es
// Wire format: e_pub[32] + EncryptAndHash(s_pub)[32+16] = 80 bytes

int noise_hs_read_msg1_write_msg2(NoiseHandshakeState *hs,
                                   const uint8_t *msg1, size_t msg1_len,
                                   uint8_t *out, size_t *out_len) {
    if (hs->phase != NOISE_HS_IDLE || hs->role != NOISE_ROLE_RESPONDER) return -1;

    // Parse msg1: raw 32-byte ephemeral key
    if (msg1_len < 32) {
        Serial.printf("[%s] msg1 too short (%d)\n", TAG, (int)msg1_len);
        hs->phase = NOISE_HS_FAILED; return -2;
    }
    memcpy(hs->re_pub, msg1, 32);

    // MixHash(re) — mix in initiator's ephemeral
    noise_mix_hash(&hs->ss, hs->re_pub, 32);

    size_t offset = 0;

    // Send: e
    memcpy(out + offset, hs->e_pub, 32);
    offset += 32;
    noise_mix_hash(&hs->ss, hs->e_pub, 32);

    // Perform: ee — DH(our ephemeral, their ephemeral)
    uint8_t dh[32];
    int ret = noise_dh(hs->e_priv, hs->re_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    // Send: s — EncryptAndHash(our static pubkey)
    size_t enc_len = 0;
    ret = noise_encrypt_and_hash(&hs->ss, hs->s_pub, 32, out + offset, &enc_len);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    offset += enc_len;  // 32 + 16 = 48 bytes

    // Perform: es — DH(our static, their ephemeral)
    ret = noise_dh(hs->s_priv, hs->re_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    memset(dh, 0, sizeof(dh));
    *out_len = offset;  // should be 80

    hs->phase = NOISE_HS_AWAIT_MSG3;
    Serial.printf("[%s] Sent msg2 (e,ee,s,es, %dB)\n", TAG, (int)offset);
    return 0;
}

// ── Message 3: Initiator → Responder ─────────────────────────────────
// Pattern tokens: s, se
// Wire format: EncryptAndHash(s_pub)[32+16] = 48 bytes

int noise_hs_read_msg2_write_msg3(NoiseHandshakeState *hs,
                                   const uint8_t *msg2, size_t msg2_len,
                                   uint8_t *out, size_t *out_len) {
    if (hs->phase != NOISE_HS_AWAIT_MSG2 || hs->role != NOISE_ROLE_INITIATOR) return -1;

    // msg2 should be 80 bytes: e[32] + enc_s[32] + tag[16]
    if (msg2_len < 80) {
        Serial.printf("[%s] msg2 too short (%d, need 80)\n", TAG, (int)msg2_len);
        hs->phase = NOISE_HS_FAILED; return -2;
    }

    size_t pos = 0;

    // Read: e — responder's ephemeral key
    memcpy(hs->re_pub, msg2 + pos, 32);
    pos += 32;
    noise_mix_hash(&hs->ss, hs->re_pub, 32);

    // Perform: ee — DH(our ephemeral, their ephemeral)
    uint8_t dh[32];
    int ret = noise_dh(hs->e_priv, hs->re_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    // Read: s — DecryptAndHash(responder's encrypted static key)
    size_t rs_len = 0;
    ret = noise_decrypt_and_hash(&hs->ss, msg2 + pos, 48, hs->rs_pub, &rs_len);
    if (ret != 0) {
        Serial.printf("[%s] msg2: decrypt rs failed (%d)\n", TAG, ret);
        hs->phase = NOISE_HS_FAILED; return ret;
    }
    pos += 48;

    // Perform: es — DH(our ephemeral, their static)
    ret = noise_dh(hs->e_priv, hs->rs_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    // Write: s — EncryptAndHash(our static pubkey)
    size_t enc_len = 0;
    ret = noise_encrypt_and_hash(&hs->ss, hs->s_pub, 32, out, &enc_len);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    // Perform: se — DH(our static, their ephemeral)
    ret = noise_dh(hs->s_priv, hs->re_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    memset(dh, 0, sizeof(dh));

    // Split() → transport keys (initiator: send=k1, recv=k2)
    ret = noise_split(&hs->ss, &hs->send_cs, &hs->recv_cs);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    *out_len = enc_len;  // should be 48
    hs->phase = NOISE_HS_TRANSPORT;
    Serial.printf("[%s] Sent msg3 (s,se, %dB) — transport mode\n", TAG, (int)enc_len);
    return 0;
}

// ── Finalise (responder processes msg3) ──────────────────────────────
// Pattern tokens: s, se
// Wire format: enc_s[32] + tag[16] = 48 bytes

int noise_hs_read_msg3(NoiseHandshakeState *hs,
                        const uint8_t *msg3, size_t msg3_len) {
    if (hs->phase != NOISE_HS_AWAIT_MSG3 || hs->role != NOISE_ROLE_RESPONDER) return -1;

    if (msg3_len < 48) {
        Serial.printf("[%s] msg3 too short (%d, need 48)\n", TAG, (int)msg3_len);
        hs->phase = NOISE_HS_FAILED; return -2;
    }

    // Read: s — DecryptAndHash(initiator's encrypted static key)
    size_t rs_len = 0;
    int ret = noise_decrypt_and_hash(&hs->ss, msg3, 48, hs->rs_pub, &rs_len);
    if (ret != 0) {
        Serial.printf("[%s] msg3: decrypt rs failed (%d)\n", TAG, ret);
        hs->phase = NOISE_HS_FAILED; return ret;
    }

    // Perform: se — DH(our ephemeral, their static)
    uint8_t dh[32];
    ret = noise_dh(hs->e_priv, hs->rs_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    memset(dh, 0, sizeof(dh));

    // Split() → transport keys (responder: reversed, recv=k1, send=k2)
    ret = noise_split(&hs->ss, &hs->recv_cs, &hs->send_cs);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    hs->phase = NOISE_HS_TRANSPORT;
    Serial.printf("[%s] Handshake complete (responder), rs=%02x%02x%02x%02x...\n",
                  TAG, hs->rs_pub[0], hs->rs_pub[1], hs->rs_pub[2], hs->rs_pub[3]);
    return 0;
}

// ── Transport encrypt/decrypt ─────────────────────────────────────────

int noise_hs_encrypt(NoiseHandshakeState *hs,
                      const uint8_t *ad, size_t ad_len,
                      const uint8_t *pt, size_t plen,
                      uint8_t *ct, size_t *ct_len) {
    if (hs->phase != NOISE_HS_TRANSPORT) return -1;
    return noise_cs_encrypt(&hs->send_cs, ad, ad_len, pt, plen, ct, ct_len);
}

int noise_hs_decrypt(NoiseHandshakeState *hs,
                      const uint8_t *ad, size_t ad_len,
                      const uint8_t *ct, size_t ct_len,
                      uint8_t *pt, size_t *pt_len) {
    if (hs->phase != NOISE_HS_TRANSPORT) return -1;
    return noise_cs_decrypt(&hs->recv_cs, ad, ad_len, ct, ct_len, pt, pt_len);
}
