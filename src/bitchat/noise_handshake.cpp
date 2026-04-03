#include "noise_handshake.h"
#include "../config.h"   // for BITCHAT_TLV_* constants
#include <Arduino.h>
#include <cstring>

static const char *TAG = "Noise";

// ── Internal TLV helpers ──────────────────────────────────────────────
// (duplicated locally to avoid dependency on BitchatBLE class)

static int _tlv_enc(uint8_t *buf, uint8_t type, const uint8_t *val, uint8_t len) {
    buf[0] = type;
    buf[1] = len;
    memcpy(buf + 2, val, len);
    return 2 + len;
}

static bool _tlv_get(const uint8_t *buf, int buf_len, uint8_t type,
                      const uint8_t **val, uint8_t *len) {
    int pos = 0;
    while (pos + 2 <= buf_len) {
        uint8_t t = buf[pos], l = buf[pos + 1];
        if (pos + 2 + l > buf_len) break;
        if (t == type) { *val = buf + pos + 2; *len = l; return true; }
        pos += 2 + l;
    }
    return false;
}

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
// Tokens: e
//   MixHash(e.pub)
//   Payload TLV: TLV_NOISE_INIT = e.pub [32 bytes]

int noise_hs_write_msg1(NoiseHandshakeState *hs, uint8_t *out, size_t *out_len) {
    if (hs->phase != NOISE_HS_IDLE || hs->role != NOISE_ROLE_INITIATOR) return -1;

    noise_mix_hash(&hs->ss, hs->e_pub, 32);                    // MixHash(e)

    int pos = 0;
    pos += _tlv_enc(out + pos, BITCHAT_TLV_NOISE_INIT, hs->e_pub, 32);
    *out_len = pos;

    hs->phase = NOISE_HS_AWAIT_MSG2;
    Serial.printf("[%s] Sent msg1 (e)\n", TAG);
    return 0;
}

// ── Message 2: Responder → Initiator ─────────────────────────────────
// Tokens: e, ee, s, es
//   MixHash(re)           (mix in initiator's ephemeral)
//   MixHash(e.pub)        (mix in our ephemeral)
//   MixKey(DH(e, re))     (ee)
//   EncryptAndHash(s.pub) (send our static, encrypted)
//   MixKey(DH(s, re))     (es: our static × their ephemeral)
// Payload TLVs: TLV_NOISE_INIT=e.pub, TLV_NOISE_RESP=EncryptAndHash(s.pub)

int noise_hs_read_msg1_write_msg2(NoiseHandshakeState *hs,
                                   const uint8_t *msg1, size_t msg1_len,
                                   uint8_t *out, size_t *out_len) {
    if (hs->phase != NOISE_HS_IDLE || hs->role != NOISE_ROLE_RESPONDER) return -1;

    // Parse msg1: TLV_NOISE_INIT = re.pub
    const uint8_t *re = nullptr; uint8_t re_len = 0;
    if (!_tlv_get(msg1, msg1_len, BITCHAT_TLV_NOISE_INIT, &re, &re_len) || re_len != 32) {
        Serial.printf("[%s] msg1: missing/bad TLV_NOISE_INIT\n", TAG);
        hs->phase = NOISE_HS_FAILED; return -2;
    }
    memcpy(hs->re_pub, re, 32);

    // MixHash(re)
    noise_mix_hash(&hs->ss, hs->re_pub, 32);
    // MixHash(e.pub)
    noise_mix_hash(&hs->ss, hs->e_pub, 32);

    // MixKey(DH(e, re))  — ee
    uint8_t dh[32];
    int ret = noise_dh(hs->e_priv, hs->re_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    // EncryptAndHash(s.pub)
    uint8_t enc_s[48]; size_t enc_s_len = 0;
    ret = noise_encrypt_and_hash(&hs->ss, hs->s_pub, 32, enc_s, &enc_s_len);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    // MixKey(DH(s, re))  — es
    ret = noise_dh(hs->s_priv, hs->re_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    memset(dh, 0, sizeof(dh));

    // Build output TLVs
    int pos = 0;
    pos += _tlv_enc(out + pos, BITCHAT_TLV_NOISE_INIT, hs->e_pub, 32);
    pos += _tlv_enc(out + pos, BITCHAT_TLV_NOISE_RESP, enc_s, (uint8_t)enc_s_len);
    *out_len = pos;

    hs->phase = NOISE_HS_AWAIT_MSG3;
    Serial.printf("[%s] Sent msg2 (e,ee,s,es)\n", TAG);
    return 0;
}

// ── Message 3: Initiator → Responder ─────────────────────────────────
// Tokens: s, se
//   (first process msg2: re, ee, decrypt rs, es)
//   EncryptAndHash(s.pub)  (send our static, encrypted)
//   MixKey(DH(s, re))      (se: our static × their ephemeral)
//   Split() → transport keys
// Payload TLVs: TLV_NOISE_FINISH=EncryptAndHash(s.pub)

int noise_hs_read_msg2_write_msg3(NoiseHandshakeState *hs,
                                   const uint8_t *msg2, size_t msg2_len,
                                   uint8_t *out, size_t *out_len) {
    if (hs->phase != NOISE_HS_AWAIT_MSG2 || hs->role != NOISE_ROLE_INITIATOR) return -1;

    // Parse msg2 TLVs
    const uint8_t *re = nullptr; uint8_t re_len = 0;
    const uint8_t *enc_s = nullptr; uint8_t enc_s_len = 0;
    if (!_tlv_get(msg2, msg2_len, BITCHAT_TLV_NOISE_INIT, &re, &re_len) || re_len != 32) {
        Serial.printf("[%s] msg2: missing/bad TLV_NOISE_INIT\n", TAG);
        hs->phase = NOISE_HS_FAILED; return -2;
    }
    if (!_tlv_get(msg2, msg2_len, BITCHAT_TLV_NOISE_RESP, &enc_s, &enc_s_len) || enc_s_len < 16) {
        Serial.printf("[%s] msg2: missing/bad TLV_NOISE_RESP\n", TAG);
        hs->phase = NOISE_HS_FAILED; return -3;
    }
    memcpy(hs->re_pub, re, 32);

    // MixHash(re)
    noise_mix_hash(&hs->ss, hs->re_pub, 32);

    // MixKey(DH(e, re))  — ee
    uint8_t dh[32];
    int ret = noise_dh(hs->e_priv, hs->re_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    // DecryptAndHash(enc_s) → rs.pub
    size_t rs_len = 0;
    ret = noise_decrypt_and_hash(&hs->ss, enc_s, enc_s_len, hs->rs_pub, &rs_len);
    if (ret != 0) {
        Serial.printf("[%s] msg2: decrypt rs failed (%d)\n", TAG, ret);
        hs->phase = NOISE_HS_FAILED; return ret;
    }

    // MixKey(DH(e, rs))  — es
    ret = noise_dh(hs->e_priv, hs->rs_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    // EncryptAndHash(s.pub)  — our static key
    uint8_t enc_my_s[48]; size_t enc_my_s_len = 0;
    ret = noise_encrypt_and_hash(&hs->ss, hs->s_pub, 32, enc_my_s, &enc_my_s_len);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    // MixKey(DH(s, re))  — se: our static × their ephemeral
    ret = noise_dh(hs->s_priv, hs->re_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    memset(dh, 0, sizeof(dh));

    // Split() — initiator: send_cs = k1, recv_cs = k2
    ret = noise_split(&hs->ss, &hs->send_cs, &hs->recv_cs);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }

    // Build output TLV
    int pos = 0;
    pos += _tlv_enc(out + pos, BITCHAT_TLV_NOISE_FINISH, enc_my_s, (uint8_t)enc_my_s_len);
    *out_len = pos;

    hs->phase = NOISE_HS_TRANSPORT;
    Serial.printf("[%s] Sent msg3, handshake complete (initiator)\n", TAG);
    return 0;
}

// ── Finalise (responder processes msg3) ──────────────────────────────
// Tokens: s, se
//   DecryptAndHash(rs.pub)
//   MixKey(DH(e, rs))  — se: responder's ephemeral × initiator's static
//   Split() — reversed: recv_cs = k1, send_cs = k2

int noise_hs_read_msg3(NoiseHandshakeState *hs,
                        const uint8_t *msg3, size_t msg3_len) {
    if (hs->phase != NOISE_HS_AWAIT_MSG3 || hs->role != NOISE_ROLE_RESPONDER) return -1;

    // Parse TLV_NOISE_FINISH
    const uint8_t *enc_s = nullptr; uint8_t enc_s_len = 0;
    if (!_tlv_get(msg3, msg3_len, BITCHAT_TLV_NOISE_FINISH, &enc_s, &enc_s_len) || enc_s_len < 16) {
        Serial.printf("[%s] msg3: missing/bad TLV_NOISE_FINISH\n", TAG);
        hs->phase = NOISE_HS_FAILED; return -2;
    }

    // DecryptAndHash(enc_s) → rs.pub (initiator's static key)
    size_t rs_len = 0;
    int ret = noise_decrypt_and_hash(&hs->ss, enc_s, enc_s_len, hs->rs_pub, &rs_len);
    if (ret != 0) {
        Serial.printf("[%s] msg3: decrypt rs failed (%d)\n", TAG, ret);
        hs->phase = NOISE_HS_FAILED; return ret;
    }

    // MixKey(DH(e, rs))  — se: our ephemeral × their static
    uint8_t dh[32];
    ret = noise_dh(hs->e_priv, hs->rs_pub, dh);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    ret = noise_mix_key(&hs->ss, dh, 32);
    if (ret != 0) { hs->phase = NOISE_HS_FAILED; return ret; }
    memset(dh, 0, sizeof(dh));

    // Split() — responder: recv_cs = k1, send_cs = k2 (reversed vs initiator)
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
