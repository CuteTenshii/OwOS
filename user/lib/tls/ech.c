/* ech.c — see ech.h. */

#include "ech.h"

#define HPKE_KEM_X25519 0x0020
#define HPKE_KDF_SHA256 0x0001
#define HPKE_AEAD_AES128GCM 0x0001

static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }

static void put16(uint8_t *p, unsigned v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

/* Parse one ECHConfig body (after version and length). 1 = usable. */
static int parse_config(const uint8_t *p, uint32_t n, struct ech_cfg *out)
{
  if (n < 1 + 2 + 2)
    return 0;
  uint32_t o = 0;
  uint8_t id = p[o++];
  unsigned kem = be16(p + o);
  o += 2;
  unsigned publen = be16(p + o);
  o += 2;
  if (o + publen + 2 > n)
    return 0;
  const uint8_t *pub = p + o;
  o += publen;
  unsigned cslen = be16(p + o);
  o += 2;
  if (o + cslen + 2 > n || (cslen & 3))
    return 0;
  int suite_ok = 0;
  for (unsigned i = 0; i < cslen; i += 4)
    if (be16(p + o + i) == HPKE_KDF_SHA256 && be16(p + o + i + 2) == HPKE_AEAD_AES128GCM)
      suite_ok = 1;
  o += cslen;
  uint8_t max_name = p[o++];
  unsigned pnlen = p[o++];
  if (!pnlen || o + pnlen + 2 > n)
    return 0;
  const uint8_t *pn = p + o;
  o += pnlen;
  unsigned extlen = be16(p + o);
  o += 2;
  if (o + extlen != n)
    return 0;
  for (unsigned e = 0; e + 4 <= extlen;) {
    unsigned type = be16(p + o + e), l = be16(p + o + e + 2);
    if (type & 0x8000) /* mandatory extension we don't implement */
      return 0;
    e += 4 + l;
  }

  if (kem != HPKE_KEM_X25519 || publen != 32 || !suite_ok)
    return 0;
  out->config_id = id;
  out->max_name_len = max_name;
  for (int i = 0; i < 32; i++)
    out->pub[i] = pub[i];
  for (unsigned i = 0; i < pnlen; i++)
    out->public_name[i] = (char)pn[i];
  out->public_name[pnlen] = '\0';
  return 1;
}

int ech_pick_config(const uint8_t *list, uint32_t len, struct ech_cfg *out)
{
  if (len < 2 || be16(list) + 2 != len)
    return 0;
  uint32_t o = 2;
  while (o + 4 <= len) {
    unsigned version = be16(list + o), l = be16(list + o + 2);
    if (o + 4 + l > len)
      return 0;
    if (version == ECH_EXT_TYPE && parse_config(list + o + 4, l, out)) {
      out->raw = list + o;
      out->rawlen = 4 + l;
      return 1;
    }
    o += 4 + l;
  }
  return 0;
}

uint32_t ech_encode_inner(const uint8_t *ch, uint32_t chlen, uint32_t sni_len, const struct ech_cfg *cfg, uint8_t *out,
                          uint32_t outmax)
{
  const uint8_t *body = ch + 4;
  uint32_t blen = chlen - 4;
  /* legacy_version(2) random(32) session_id */
  if (chlen < 4 + 35 || body[34] > blen - 35)
    return 0;
  uint32_t sid = body[34], tail = blen - 35 - sid;
  uint32_t len = 35 + tail; /* prefix, empty session id, rest */
  uint32_t name_pad = cfg->max_name_len > sni_len ? cfg->max_name_len - sni_len : 0;
  uint32_t pad = name_pad + 31 - ((len + name_pad - 1) % 32);
  if (len + pad > outmax)
    return 0;
  for (uint32_t i = 0; i < 34; i++)
    out[i] = body[i];
  out[34] = 0;
  for (uint32_t i = 0; i < tail; i++)
    out[35 + i] = body[35 + sid + i];
  for (uint32_t i = 0; i < pad; i++)
    out[len + i] = 0;
  return len + pad;
}

uint32_t ech_outer_ext(uint8_t *out, const struct ech_cfg *cfg, const uint8_t enc[32], uint32_t payload_len)
{
  uint8_t *p = out;
  put16(p, ECH_EXT_TYPE);
  put16(p + 2, 1 + 4 + 1 + 2 + 32 + 2 + payload_len);
  p += 4;
  *p++ = 0; /* ECHClientHelloType outer */
  put16(p, HPKE_KDF_SHA256);
  put16(p + 2, HPKE_AEAD_AES128GCM);
  p += 4;
  *p++ = cfg->config_id;
  put16(p, 32);
  p += 2;
  for (int i = 0; i < 32; i++)
    *p++ = enc[i];
  put16(p, payload_len);
  p += 2;
  for (uint32_t i = 0; i < payload_len; i++)
    *p++ = 0;
  return (uint32_t)(p - out);
}
