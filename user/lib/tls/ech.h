/* ech.h — Encrypted Client Hello (ECH) wire helpers: ECHConfig selection and the
 * ClientHelloInner / outer-extension encodings. Pure byte manipulation. */
#ifndef OWOS_USER_ECH_H
#define OWOS_USER_ECH_H

#include <stdint.h>

#define ECH_EXT_TYPE 0xfe0d
#define ECH_PUBLIC_NAME_MAX 255

/* A usable ECHConfig: HPKE suite DHKEM(X25519)/HKDF-SHA256/AES-128-GCM, no mandatory extensions. */
struct ech_cfg {
  const uint8_t *raw; /* the whole ECHConfig (version and length included), points into the list */
  uint32_t rawlen;
  uint8_t config_id;
  uint8_t max_name_len;
  uint8_t pub[32];
  char public_name[ECH_PUBLIC_NAME_MAX + 1];
};

/* Pick the first usable config in an ECHConfigList. 1 = found, 0 = none. */
int ech_pick_config(const uint8_t *list, uint32_t len, struct ech_cfg *out);

/* Encode a ClientHelloInner handshake message (4-byte header included) as EncodedClientHelloInner:
 * header dropped, legacy_session_id emptied, zero padding appended (RFC 9849 §6.1.3). Returns the
 * length, or 0 if it is malformed or does not fit in outmax. */
uint32_t ech_encode_inner(const uint8_t *ch, uint32_t chlen, uint32_t sni_len, const struct ech_cfg *cfg, uint8_t *out,
                          uint32_t outmax);

/* Write the outer encrypted_client_hello extension (header included) with a zeroed payload of
 * payload_len bytes at the end. Returns the extension's total length. */
uint32_t ech_outer_ext(uint8_t *out, const struct ech_cfg *cfg, const uint8_t enc[32], uint32_t payload_len);

#endif /* OWOS_USER_ECH_H */
