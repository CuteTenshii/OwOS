/* x509.c — see x509.h. */

#include "x509.h"
#include "ed25519.h"
#include "owos.h" /* memcpy/memset/malloc (user libc) */
#include "pk.h"

#define MAX_CHAIN 16

/* ------------------------------------------------------------------ DER */

struct tlv {
  uint8_t tag;
  const uint8_t *start, *val;
  uint32_t vlen;
};

static int tlv_read(const uint8_t **pp, const uint8_t *end, struct tlv *t)
{
  const uint8_t *p = *pp;
  if (end - p < 2)
    return -1;
  t->start = p;
  t->tag = *p++;
  if ((t->tag & 0x1f) == 0x1f)
    return -1;
  uint32_t len = *p++;
  if (len & 0x80) {
    uint32_t nb = len & 0x7f;
    if (nb == 0 || nb > 3 || (uint32_t)(end - p) < nb)
      return -1;
    for (len = 0; nb--;)
      len = len << 8 | *p++;
  }
  if (len > (uint32_t)(end - p))
    return -1;
  t->val = p;
  t->vlen = len;
  *pp = p + len;
  return 0;
}

static int tlv_expect(const uint8_t **pp, const uint8_t *end, uint8_t tag, struct tlv *t)
{
  return tlv_read(pp, end, t) || t->tag != tag ? -1 : 0;
}

static uint32_t tlv_total(const struct tlv *t) { return (uint32_t)(t->val - t->start) + t->vlen; }

#define OID_EQ(t, lit) ((t).vlen == sizeof(lit) && !memcmp((t).val, lit, sizeof(lit)))

static const uint8_t OID_RSA[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01};
static const uint8_t OID_RSA_SHA256[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B};
static const uint8_t OID_RSA_SHA384[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C};
static const uint8_t OID_RSA_SHA512[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D};
static const uint8_t OID_RSA_PSS[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0A};
static const uint8_t OID_MGF1[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x08};
static const uint8_t OID_SHA256[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01};
static const uint8_t OID_SHA384[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02};
static const uint8_t OID_SHA512[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03};
static const uint8_t OID_NAME_CONSTRAINTS[] = {0x55, 0x1D, 0x1E};
static const uint8_t OID_EC[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
static const uint8_t OID_P256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
static const uint8_t OID_P384[] = {0x2B, 0x81, 0x04, 0x00, 0x22};
static const uint8_t OID_ECDSA_SHA256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
static const uint8_t OID_ECDSA_SHA384[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03};
static const uint8_t OID_ECDSA_SHA512[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x04};
static const uint8_t OID_ED25519[] = {0x2B, 0x65, 0x70};
static const uint8_t OID_BASIC[] = {0x55, 0x1D, 0x13};
static const uint8_t OID_KEYUSAGE[] = {0x55, 0x1D, 0x0F};
static const uint8_t OID_SAN[] = {0x55, 0x1D, 0x11};
static const uint8_t OID_EKU[] = {0x55, 0x1D, 0x25};
static const uint8_t OID_POLICIES[] = {0x55, 0x1D, 0x20};
static const uint8_t OID_CRLDP[] = {0x55, 0x1D, 0x1F};
static const uint8_t OID_SKI[] = {0x55, 0x1D, 0x0E};
static const uint8_t OID_AKI[] = {0x55, 0x1D, 0x23};
static const uint8_t OID_SERVER_AUTH[] = {0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01};
static const uint8_t OID_ANY_EKU[] = {0x55, 0x1D, 0x25, 0x00};

const char *x509_strerror(int err)
{
  static const char *const names[] = {"ok",
                                      "malformed certificate",
                                      "unsupported algorithm or extension",
                                      "weak key",
                                      "untrusted issuer",
                                      "bad signature",
                                      "certificate expired",
                                      "certificate not yet valid",
                                      "issuer is not a CA",
                                      "hostname mismatch",
                                      "certificate not valid for servers",
                                      "no trusted root certificates installed",
                                      "name constraint violated"};
  return err >= 0 && err < (int)(sizeof names / sizeof *names) ? names[err] : "unknown error";
}

/* ----------------------------------------------------------------- time */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static int digits(const uint8_t *p, int n)
{
  int v = 0;
  while (n--) {
    if (*p < '0' || *p > '9')
      return -1;
    v = v * 10 + (*p++ - '0');
  }
  return v;
}

/* UTCTime YYMMDDHHMMSSZ or GeneralizedTime YYYYMMDDHHMMSSZ. */
static int parse_time(const struct tlv *t, int64_t *out)
{
  int yl = t->tag == 0x17 ? 2 : 4;
  if ((t->tag != 0x17 && t->tag != 0x18) || t->vlen != (uint32_t)yl + 11 || t->val[yl + 10] != 'Z')
    return -1;
  int y = digits(t->val, yl), mo = digits(t->val + yl, 2), d = digits(t->val + yl + 2, 2);
  int h = digits(t->val + yl + 4, 2), mi = digits(t->val + yl + 6, 2), s = digits(t->val + yl + 8, 2);
  if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60)
    return -1;
  if (yl == 2)
    y += y >= 50 ? 1900 : 2000;
  *out = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s;
  return 0;
}

/* ---------------------------------------------------------------- parse */

/* A hash AlgorithmIdentifier (SHA-2 only) to pk_hash, or -1. */
static int hash_of_alg(const struct tlv *alg)
{
  const uint8_t *p = alg->val;
  struct tlv oid;
  if (tlv_expect(&p, alg->val + alg->vlen, 0x06, &oid))
    return -1;
  return OID_EQ(oid, OID_SHA256)   ? PK_SHA256
         : OID_EQ(oid, OID_SHA384) ? PK_SHA384
         : OID_EQ(oid, OID_SHA512) ? PK_SHA512
                                   : -1;
}

/* RSASSA-PSS-params (RFC 4055): hash, MGF1 over the same hash, salt length. SHA-1 defaults are refused. */
static int parse_pss_params(struct x509 *c, const uint8_t *p, const uint8_t *end)
{
  struct tlv params, f, inner;
  if (tlv_expect(&p, end, 0x30, &params))
    return -1;
  p = params.val;
  end = p + params.vlen;
  int hash = -1, mgf_hash = -1;
  uint32_t salt = 20;
  while (p < end) {
    if (tlv_read(&p, end, &f))
      return -1;
    const uint8_t *q = f.val, *qend = q + f.vlen;
    if (f.tag == 0xA0) {
      if (tlv_expect(&q, qend, 0x30, &inner))
        return -1;
      hash = hash_of_alg(&inner);
    } else if (f.tag == 0xA1) {
      struct tlv oid, h;
      if (tlv_expect(&q, qend, 0x30, &inner))
        return -1;
      const uint8_t *m = inner.val, *mend = m + inner.vlen;
      if (tlv_expect(&m, mend, 0x06, &oid) || !OID_EQ(oid, OID_MGF1) || tlv_expect(&m, mend, 0x30, &h))
        return -1;
      mgf_hash = hash_of_alg(&h);
    } else if (f.tag == 0xA2) {
      if (tlv_expect(&q, qend, 0x02, &inner) || inner.vlen != 1 || inner.val[0] > 64)
        return -1;
      salt = inner.val[0];
    }
  }
  if (hash < 0 || hash != mgf_hash)
    return -1;
  c->pss_hash = hash;
  c->pss_saltlen = salt;
  return 0;
}

static int sig_alg_of(struct x509 *c, const struct tlv *alg)
{
  const uint8_t *p = alg->val, *end = p + alg->vlen;
  struct tlv oid;
  if (tlv_expect(&p, end, 0x06, &oid))
    return X509_SIG_NONE;
  if (OID_EQ(oid, OID_RSA_SHA256))
    return X509_SIG_RSA_SHA256;
  if (OID_EQ(oid, OID_RSA_SHA384))
    return X509_SIG_RSA_SHA384;
  if (OID_EQ(oid, OID_RSA_SHA512))
    return X509_SIG_RSA_SHA512;
  if (OID_EQ(oid, OID_ECDSA_SHA256))
    return X509_SIG_ECDSA_SHA256;
  if (OID_EQ(oid, OID_ECDSA_SHA384))
    return X509_SIG_ECDSA_SHA384;
  if (OID_EQ(oid, OID_ECDSA_SHA512))
    return X509_SIG_ECDSA_SHA512;
  if (OID_EQ(oid, OID_ED25519))
    return X509_SIG_ED25519;
  if (OID_EQ(oid, OID_RSA_PSS))
    return parse_pss_params(c, p, end) ? X509_SIG_NONE : X509_SIG_RSA_PSS;
  return X509_SIG_NONE;
}

static int parse_spki(struct x509 *c, const struct tlv *spki)
{
  const uint8_t *p = spki->val, *end = p + spki->vlen;
  struct tlv alg, bits, oid;
  if (tlv_expect(&p, end, 0x30, &alg) || tlv_expect(&p, end, 0x03, &bits) || p != end || bits.vlen < 1 ||
      bits.val[0] != 0)
    return -1;
  const uint8_t *a = alg.val, *aend = a + alg.vlen;
  if (tlv_expect(&a, aend, 0x06, &oid))
    return -1;
  const uint8_t *key = bits.val + 1;
  uint32_t keylen = bits.vlen - 1;

  if (OID_EQ(oid, OID_RSA)) {
    struct tlv seq, n, e;
    const uint8_t *k = key, *kend = key + keylen;
    if (tlv_expect(&k, kend, 0x30, &seq) || k != kend)
      return -1;
    k = seq.val;
    kend = k + seq.vlen;
    if (tlv_expect(&k, kend, 0x02, &n) || tlv_expect(&k, kend, 0x02, &e) || k != kend || !n.vlen || !e.vlen)
      return -1;
    while (n.vlen > 1 && !*n.val) {
      n.val++;
      n.vlen--;
    }
    c->key_type = X509_KEY_RSA;
    c->key = n.val;
    c->keylen = n.vlen;
    c->rsa_e = e.val;
    c->rsa_elen = e.vlen;
  } else if (OID_EQ(oid, OID_EC)) {
    struct tlv curve;
    if (tlv_expect(&a, aend, 0x06, &curve) || key[0] != 4)
      return -1;
    if (OID_EQ(curve, OID_P256) && keylen == 65)
      c->key_type = X509_KEY_P256;
    else if (OID_EQ(curve, OID_P384) && keylen == 97)
      c->key_type = X509_KEY_P384;
    else
      return 0; /* another curve: parses, but cannot be used */
    c->key = key;
    c->keylen = keylen;
  } else if (OID_EQ(oid, OID_ED25519) && keylen == 32) {
    c->key_type = X509_KEY_ED25519;
    c->key = key;
    c->keylen = keylen;
  }
  return 0;
}

static int parse_extension(struct x509 *c, const struct tlv *ext)
{
  const uint8_t *p = ext->val, *end = p + ext->vlen;
  struct tlv oid, val;
  int critical = 0;
  if (tlv_expect(&p, end, 0x06, &oid) || tlv_read(&p, end, &val))
    return -1;
  if (val.tag == 0x01) {
    if (val.vlen != 1)
      return -1;
    critical = val.val[0] != 0;
    if (tlv_read(&p, end, &val))
      return -1;
  }
  if (val.tag != 0x04 || p != end)
    return -1;
  const uint8_t *v = val.val, *vend = v + val.vlen;
  struct tlv t;

  if (OID_EQ(oid, OID_BASIC)) {
    if (tlv_expect(&v, vend, 0x30, &t))
      return -1;
    v = t.val;
    vend = v + t.vlen;
    struct tlv f = {0};
    if (v < vend && tlv_read(&v, vend, &f))
      return -1;
    if (f.tag == 0x01) { /* optional cA BOOLEAN */
      if (f.vlen != 1)
        return -1;
      c->is_ca = f.val[0] != 0;
      f.tag = 0;
      if (v < vend && tlv_read(&v, vend, &f))
        return -1;
    }
    if (f.tag == 0x02 && f.vlen >= 1 && f.vlen <= 2 && !(f.val[0] & 0x80))
      c->pathlen = f.vlen == 1 ? f.val[0] : f.val[0] << 8 | f.val[1];
  } else if (OID_EQ(oid, OID_KEYUSAGE)) {
    if (tlv_expect(&v, vend, 0x03, &t) || t.vlen < 2)
      return -1;
    c->has_ku = 1;
    c->ku_cert_sign = (t.val[1] & 0x04) != 0;
  } else if (OID_EQ(oid, OID_SAN)) {
    if (tlv_expect(&v, vend, 0x30, &t))
      return -1;
    c->san = t.val;
    c->sanlen = t.vlen;
  } else if (OID_EQ(oid, OID_NAME_CONSTRAINTS)) {
    if (tlv_expect(&v, vend, 0x30, &t))
      return -1;
    c->nc = t.val;
    c->nclen = t.vlen;
  } else if (OID_EQ(oid, OID_EKU)) {
    if (tlv_expect(&v, vend, 0x30, &t))
      return -1;
    c->has_eku = 1;
    const uint8_t *e = t.val, *eend = e + t.vlen;
    struct tlv purpose;
    while (e < eend) {
      if (tlv_expect(&e, eend, 0x06, &purpose))
        return -1;
      if (OID_EQ(purpose, OID_SERVER_AUTH) || OID_EQ(purpose, OID_ANY_EKU))
        c->eku_server_auth = 1;
    }
  } else if (critical && !OID_EQ(oid, OID_POLICIES) && !OID_EQ(oid, OID_CRLDP) && !OID_EQ(oid, OID_SKI) &&
             !OID_EQ(oid, OID_AKI)) {
    c->unsupported_critical = 1;
  }
  return 0;
}

int x509_parse(struct x509 *c, const uint8_t *der, uint32_t len)
{
  memset(c, 0, sizeof *c);
  c->der = der;
  c->derlen = len;
  c->pathlen = -1;

  const uint8_t *p = der, *end = der + len;
  struct tlv cert, tbs, outer_alg, sigbits, t;
  if (tlv_expect(&p, end, 0x30, &cert) || p != end)
    return X509_E_PARSE;
  p = cert.val;
  end = p + cert.vlen;
  if (tlv_expect(&p, end, 0x30, &tbs) || tlv_expect(&p, end, 0x30, &outer_alg) || tlv_expect(&p, end, 0x03, &sigbits) ||
      p != end || sigbits.vlen < 1 || sigbits.val[0] != 0)
    return X509_E_PARSE;
  c->tbs = tbs.start;
  c->tbslen = tlv_total(&tbs);
  c->sig = sigbits.val + 1;
  c->siglen = sigbits.vlen - 1;

  p = tbs.val;
  end = p + tbs.vlen;
  if (tlv_read(&p, end, &t))
    return X509_E_PARSE;
  if (t.tag == 0xA0 && tlv_read(&p, end, &t)) /* version */
    return X509_E_PARSE;
  struct tlv inner_alg, issuer, validity, subject, spki, nb, na;
  if (t.tag != 0x02 || tlv_expect(&p, end, 0x30, &inner_alg) || inner_alg.vlen != outer_alg.vlen ||
      memcmp(inner_alg.val, outer_alg.val, inner_alg.vlen))
    return X509_E_PARSE;
  c->sig_alg = sig_alg_of(c, &inner_alg);

  if (tlv_expect(&p, end, 0x30, &issuer) || tlv_expect(&p, end, 0x30, &validity) ||
      tlv_expect(&p, end, 0x30, &subject) || tlv_expect(&p, end, 0x30, &spki))
    return X509_E_PARSE;
  c->issuer = issuer.start;
  c->issuerlen = tlv_total(&issuer);
  c->subject = subject.start;
  c->subjectlen = tlv_total(&subject);

  const uint8_t *v = validity.val, *vend = v + validity.vlen;
  if (tlv_read(&v, vend, &nb) || tlv_read(&v, vend, &na) || parse_time(&nb, &c->not_before) ||
      parse_time(&na, &c->not_after))
    return X509_E_PARSE;
  if (parse_spki(c, &spki))
    return X509_E_PARSE;

  while (p < end) {
    if (tlv_read(&p, end, &t))
      return X509_E_PARSE;
    if (t.tag != 0xA3)
      continue; /* issuerUniqueID / subjectUniqueID */
    const uint8_t *e = t.val, *eend = e + t.vlen;
    struct tlv list, ext;
    if (tlv_expect(&e, eend, 0x30, &list))
      return X509_E_PARSE;
    e = list.val;
    eend = e + list.vlen;
    while (e < eend)
      if (tlv_expect(&e, eend, 0x30, &ext) || parse_extension(c, &ext))
        return X509_E_PARSE;
  }
  return X509_OK;
}

/* ---------------------------------------------------------------- store */

static int b64val(uint8_t c)
{
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}

static const uint8_t *find(const uint8_t *p, const uint8_t *end, const char *needle, uint32_t nl)
{
  for (; (uint32_t)(end - p) >= nl; p++)
    if (!memcmp(p, needle, nl))
      return p;
  return 0;
}

#define PEM_BEGIN "-----BEGIN CERTIFICATE-----"
#define PEM_END "-----END CERTIFICATE-----"

uint32_t x509_store_load_pem(struct x509_store *s, const uint8_t *pem, uint32_t len)
{
  const uint8_t *end = pem + len, *p;
  uint32_t blocks = 0;
  for (p = pem; (p = find(p, end, PEM_BEGIN, sizeof(PEM_BEGIN) - 1)); p += sizeof(PEM_BEGIN) - 1)
    blocks++;
  memset(s, 0, sizeof *s);
  if (!blocks)
    return 0;
  s->certs = malloc(blocks * sizeof *s->certs);
  s->der = malloc(len / 4 * 3 + 16);
  if (!s->certs || !s->der) {
    x509_store_free(s);
    return 0;
  }

  uint8_t *out = s->der;
  for (p = pem; (p = find(p, end, PEM_BEGIN, sizeof(PEM_BEGIN) - 1)) && s->count < blocks;) {
    p += sizeof(PEM_BEGIN) - 1;
    const uint8_t *stop = find(p, end, PEM_END, sizeof(PEM_END) - 1);
    if (!stop)
      break;
    uint8_t *start = out;
    uint32_t acc = 0;
    int bits = 0;
    for (; p < stop; p++) {
      int v = b64val(*p);
      if (v < 0)
        continue;
      acc = acc << 6 | (uint32_t)v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        *out++ = (uint8_t)(acc >> bits);
      }
    }
    p = stop + sizeof(PEM_END) - 1;
    struct x509 *c = &s->certs[s->count];
    if (x509_parse(c, start, (uint32_t)(out - start)) == X509_OK && c->key_type != X509_KEY_NONE)
      s->count++;
    else
      out = start;
  }
  return s->count;
}

void x509_store_free(struct x509_store *s)
{
  free(s->certs);
  free(s->der);
  memset(s, 0, sizeof *s);
}

/* ------------------------------------------------------------- hostname */

static int fold(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int equal_nocase(const uint8_t *a, const char *b, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
    if (fold(a[i]) != fold((uint8_t)b[i]))
      return 0;
  return 1;
}

static int dns_name_match(const uint8_t *pat, uint32_t pl, const char *host, uint32_t hl)
{
  if (pl > 2 && pat[0] == '*' && pat[1] == '.') {
    uint32_t dot = 0;
    while (dot < hl && host[dot] != '.')
      dot++;
    uint32_t rest = pl - 1; /* ".example.com" */
    int inner_dot = 0;
    for (uint32_t i = 2; i < pl; i++)
      inner_dot |= pat[i] == '.';
    return dot > 0 && dot < hl && inner_dot && hl - dot == rest && equal_nocase(pat + 1, host + dot, rest);
  }
  return pl == hl && equal_nocase(pat, host, hl);
}

/* Parse a dotted-quad IPv4 literal. */
static int parse_ipv4(const char *s, uint32_t n, uint8_t ip[4])
{
  uint32_t v = 0, parts = 0, digits_seen = 0;
  for (uint32_t i = 0; i <= n; i++) {
    if (i < n && s[i] >= '0' && s[i] <= '9') {
      v = v * 10 + (uint32_t)(s[i] - '0');
      if (++digits_seen > 3 || v > 255)
        return 0;
    } else if ((i == n || s[i] == '.') && digits_seen && parts < 4) {
      ip[parts++] = (uint8_t)v;
      v = digits_seen = 0;
    } else {
      return 0;
    }
  }
  return parts == 4;
}

static int host_match(const struct x509 *leaf, const char *host)
{
  uint32_t hl = 0;
  while (host[hl])
    hl++;
  if (hl && host[hl - 1] == '.')
    hl--;
  if (!hl)
    return 0;
  uint8_t ip[4];
  int is_ip = parse_ipv4(host, hl, ip);

  const uint8_t *p = leaf->san, *end = p + leaf->sanlen;
  struct tlv name;
  while (p < end && !tlv_read(&p, end, &name)) {
    if (is_ip ? (name.tag == 0x87 && name.vlen == 4 && !memcmp(name.val, ip, 4))
              : (name.tag == 0x82 && dns_name_match(name.val, name.vlen, host, hl)))
      return 1;
  }
  return 0;
}

/* ----------------------------------------------------------- signatures */

int x509_check_signature(const struct x509 *c, const struct x509 *issuer)
{
  static const int hash_of[] = {PK_SHA256, PK_SHA256, PK_SHA384, PK_SHA512, PK_SHA256, PK_SHA384, PK_SHA512};
  uint8_t digest[PK_HASH_MAX];
  if (c->sig_alg == X509_SIG_NONE)
    return X509_E_UNSUPPORTED;
  if (c->sig_alg == X509_SIG_RSA_PSS) {
    if (issuer->key_type != X509_KEY_RSA)
      return X509_E_SIG;
    if (issuer->keylen < 256)
      return X509_E_WEAK;
    pk_hash(c->pss_hash, c->tbs, c->tbslen, digest);
    return rsa_verify_pss(issuer->key, issuer->keylen, issuer->rsa_e, issuer->rsa_elen, c->pss_hash, digest,
                          c->pss_saltlen, c->sig, c->siglen)
               ? X509_E_SIG
               : X509_OK;
  }
  if (c->sig_alg == X509_SIG_ED25519) {
    return issuer->key_type == X509_KEY_ED25519 && c->siglen == 64 &&
                   !ed25519_verify(c->sig, c->tbs, c->tbslen, issuer->key)
               ? X509_OK
               : X509_E_SIG;
  }
  int alg = hash_of[c->sig_alg];
  pk_hash(alg, c->tbs, c->tbslen, digest);
  if (c->sig_alg <= X509_SIG_RSA_SHA512) {
    if (issuer->key_type != X509_KEY_RSA)
      return X509_E_SIG;
    if (issuer->keylen < 256)
      return X509_E_WEAK;
    return rsa_verify_pkcs1(issuer->key, issuer->keylen, issuer->rsa_e, issuer->rsa_elen, alg, digest, c->sig,
                            c->siglen)
               ? X509_E_SIG
               : X509_OK;
  }
  if (issuer->key_type != X509_KEY_P256 && issuer->key_type != X509_KEY_P384)
    return X509_E_SIG;
  return ecdsa_verify_der(issuer->key_type == X509_KEY_P256 ? PK_P256 : PK_P384, issuer->key, issuer->keylen, digest,
                          pk_hash_len(alg), c->sig, c->siglen)
             ? X509_E_SIG
             : X509_OK;
}

int x509_verify_tls_sig(const struct x509 *leaf, uint16_t scheme, const uint8_t *msg, uint32_t msglen,
                        const uint8_t *sig, uint32_t siglen)
{
  uint8_t digest[PK_HASH_MAX];
  int alg;
  switch (scheme) {
  case 0x0804:
  case 0x0401:
    alg = PK_SHA256;
    break;
  case 0x0805:
  case 0x0501:
    alg = PK_SHA384;
    break;
  case 0x0806:
  case 0x0601:
    alg = PK_SHA512;
    break;
  case 0x0403:
    alg = PK_SHA256;
    break;
  case 0x0503:
    alg = PK_SHA384;
    break;
  case 0x0807:
    return leaf->key_type == X509_KEY_ED25519 && siglen == 64 && !ed25519_verify(sig, msg, msglen, leaf->key)
               ? X509_OK
               : X509_E_SIG;
  default:
    return X509_E_UNSUPPORTED;
  }
  pk_hash(alg, msg, msglen, digest);
  uint32_t dl = pk_hash_len(alg);
  if (scheme == 0x0403 || scheme == 0x0503) {
    int curve = scheme == 0x0403 ? X509_KEY_P256 : X509_KEY_P384;
    return leaf->key_type == curve && !ecdsa_verify_der(curve == X509_KEY_P256 ? PK_P256 : PK_P384, leaf->key,
                                                        leaf->keylen, digest, dl, sig, siglen)
               ? X509_OK
               : X509_E_SIG;
  }
  if (leaf->key_type != X509_KEY_RSA || leaf->keylen < 256)
    return X509_E_SIG;
  int bad = (scheme >> 8) == 0x08
                ? rsa_verify_pss(leaf->key, leaf->keylen, leaf->rsa_e, leaf->rsa_elen, alg, digest, dl, sig, siglen)
                : rsa_verify_pkcs1(leaf->key, leaf->keylen, leaf->rsa_e, leaf->rsa_elen, alg, digest, sig, siglen);
  return bad ? X509_E_SIG : X509_OK;
}

/* ------------------------------------------------------- name constraints */

/* Is dNSName `name` inside the constraint `cons` (the name itself or any subdomain)? A wildcard name
 * stands for any single label under its base; an excluded subtree also catches a wildcard whose
 * base contains it. */
static int dns_in_subtree(const uint8_t *cons, uint32_t cl, const uint8_t *name, uint32_t nl, int excluded)
{
  int wild = nl > 2 && name[0] == '*' && name[1] == '.';
  int leading = cl && cons[0] == '.';
  if (leading) {
    cons++;
    cl--;
  }
  if (!cl)
    return 1;
  const uint8_t *n = wild ? name + 1 : name; /* ".example.com" stands in for "a.example.com" */
  uint32_t len = wild ? nl - 1 : nl;
  if (len >= cl && equal_nocase(n + len - cl, (const char *)cons, cl) &&
      (len == cl ? !leading : n[len - cl - 1] == '.'))
    return 1;
  /* constraint at or below the wildcard's base: e.g. excluded bad.example.com vs *.example.com */
  return excluded && wild && cl >= nl - 2 && equal_nocase(cons + cl - (nl - 2), (const char *)name + 2, nl - 2) &&
         (cl == nl - 2 || cons[cl - (nl - 2) - 1] == '.');
}

static int ip_in_subtree(const uint8_t *cons, uint32_t cl, const uint8_t *ip, uint32_t il)
{
  if (cl != 2 * il)
    return 0;
  for (uint32_t i = 0; i < il; i++)
    if ((ip[i] ^ cons[i]) & cons[il + i])
      return 0;
  return 1;
}

static int nc_check_name(const struct x509 *ca, uint8_t tag, const uint8_t *name, uint32_t nl)
{
  int has_permitted = 0, permitted = 0;
  const uint8_t *p = ca->nc, *end = p + ca->nclen;
  struct tlv group, sub, base;
  while (p < end) {
    if (tlv_read(&p, end, &group) || (group.tag != 0xA0 && group.tag != 0xA1))
      return X509_E_PARSE;
    const uint8_t *g = group.val, *gend = g + group.vlen;
    while (g < gend) {
      if (tlv_expect(&g, gend, 0x30, &sub))
        return X509_E_PARSE;
      const uint8_t *s = sub.val;
      if (tlv_read(&s, sub.val + sub.vlen, &base))
        return X509_E_PARSE;
      if (base.tag != tag)
        continue;
      int excluded = group.tag == 0xA1;
      int in = tag == 0x82 ? dns_in_subtree(base.val, base.vlen, name, nl, excluded)
                           : ip_in_subtree(base.val, base.vlen, name, nl);
      if (excluded && in)
        return X509_E_NAME_CONSTRAINT;
      if (!excluded) {
        has_permitted = 1;
        permitted |= in;
      }
    }
  }
  return has_permitted && !permitted ? X509_E_NAME_CONSTRAINT : X509_OK;
}

/* The leaf's DNS and IP names must satisfy the constraints `ca` imposes on everything below it. */
static int names_ok(const struct x509 *ca, const struct x509 *leaf)
{
  if (!ca->nc)
    return X509_OK;
  const uint8_t *p = leaf->san, *end = p + leaf->sanlen;
  struct tlv name;
  while (p < end && !tlv_read(&p, end, &name)) {
    if (name.tag != 0x82 && name.tag != 0x87)
      continue;
    int rc = nc_check_name(ca, name.tag, name.val, name.vlen);
    if (rc)
      return rc;
  }
  return X509_OK;
}

/* ---------------------------------------------------------------- chain */

static int same_bytes(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl)
{
  return al == bl && !memcmp(a, b, al);
}

static int check_dates(const struct x509 *c, int64_t now)
{
  if (!now)
    return X509_OK;
  return now < c->not_before ? X509_E_NOT_YET : now > c->not_after ? X509_E_EXPIRED : X509_OK;
}

/* May `ca` sign a certificate with `below` intermediates under it? Anchors skip the CA flag check. */
static int ca_allows(const struct x509 *ca, int anchor, int below)
{
  if (!anchor && !ca->is_ca)
    return X509_E_NOT_CA;
  if (ca->has_ku && !ca->ku_cert_sign)
    return X509_E_NOT_CA;
  if (ca->pathlen >= 0 && below > ca->pathlen)
    return X509_E_NOT_CA;
  return X509_OK;
}

int x509_verify_chain(const struct x509_store *store, const struct x509 *certs, uint32_t n, const char *host,
                      int64_t now)
{
  if (!n || n > MAX_CHAIN)
    return X509_E_PARSE;
  const struct x509 *leaf = &certs[0];
  if (leaf->has_eku && !leaf->eku_server_auth)
    return X509_E_PURPOSE;

  uint32_t used = 1;
  const struct x509 *cur = leaf;
  int err = X509_E_UNTRUSTED;
  for (int depth = 0; depth <= MAX_CHAIN; depth++) {
    int rc = check_dates(cur, now);
    if (rc)
      return rc;
    if (cur->unsupported_critical)
      return X509_E_UNSUPPORTED;

    for (uint32_t i = 0; store && i < store->count; i++) {
      const struct x509 *a = &store->certs[i];
      if (same_bytes(a->der, a->derlen, cur->der, cur->derlen))
        return host_match(leaf, host) ? X509_OK : X509_E_HOSTNAME;
    }
    for (uint32_t i = 0; store && i < store->count; i++) {
      const struct x509 *a = &store->certs[i];
      if (!same_bytes(a->subject, a->subjectlen, cur->issuer, cur->issuerlen))
        continue;
      int rc2 = ca_allows(a, 1, depth);
      if (!rc2)
        rc2 = x509_check_signature(cur, a);
      if (!rc2)
        rc2 = names_ok(a, leaf);
      if (!rc2)
        rc2 = check_dates(a, now);
      if (!rc2)
        return host_match(leaf, host) ? X509_OK : X509_E_HOSTNAME;
      err = rc2;
    }

    const struct x509 *next = 0;
    for (uint32_t j = 1; j < n; j++) {
      const struct x509 *cand = &certs[j];
      if (used >> j & 1 || !same_bytes(cand->subject, cand->subjectlen, cur->issuer, cur->issuerlen))
        continue;
      int rc2 = ca_allows(cand, 0, depth);
      if (!rc2)
        rc2 = x509_check_signature(cur, cand);
      if (!rc2)
        rc2 = names_ok(cand, leaf);
      if (rc2) {
        err = rc2;
        continue;
      }
      used |= 1u << j;
      next = cand;
      break;
    }
    if (!next)
      return err;
    cur = next;
  }
  return X509_E_UNTRUSTED;
}
