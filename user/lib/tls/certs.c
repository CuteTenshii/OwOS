/* certs.c — server certificate checking. See certs.h. */

#include "certs.h"
#include "mutex.h"
#include "owos.h"

#define CERTS_ROOTS_PATH "/etc/ssl/certs.pem"
#define CERTS_ROOTS_MAX (768 * 1024)

static mutex_t g_roots_lock = MUTEX_INIT;
static struct x509_store g_roots;
static const struct x509_store *g_roots_override;
static int g_roots_loaded; /* 0 not tried, 1 loaded, -1 unavailable */

void certs_set_roots(const struct x509_store *roots) { g_roots_override = roots; }

/* The root store, or NULL while the bundle is unavailable. */
static const struct x509_store *roots(void)
{
  if (g_roots_override)
    return g_roots_override;
  mutex_lock(&g_roots_lock);
  if (!g_roots_loaded) {
    g_roots_loaded = -1;
    uint8_t *pem = malloc(CERTS_ROOTS_MAX);
    if (pem) {
      int n = readfile(CERTS_ROOTS_PATH, (char *)pem, CERTS_ROOTS_MAX);
      if (n > 0 && x509_store_load_pem(&g_roots, pem, (uint32_t)n))
        g_roots_loaded = 1;
      else
        x509_store_free(&g_roots);
      free(pem);
    }
  }
  int ok = g_roots_loaded > 0;
  mutex_unlock(&g_roots_lock);
  return ok ? &g_roots : 0;
}

/* Unix seconds, or 0 while the clock is unset (the date checks are skipped then). */
static int64_t wall_now(void)
{
  struct timeval64 tv;
  return gettime(&tv) == 0 && tv.tv_sec >= 1704067200 ? tv.tv_sec : 0;
}

void certs_fail(struct certs *c, int err)
{
  if (c->verify == X509_OK)
    c->verify = err;
}

void certs_init(struct certs *c)
{
  c->verify = X509_E_UNTRUSTED;
  c->nchain = 0;
}

void certs_take(struct certs *c, const char *host, const uint8_t *body, uint32_t bl, int tls13)
{
  c->nchain = 0;
  c->verify = X509_E_PARSE;
  uint32_t o = tls13 ? 1u + (bl ? body[0] : 0) : 0;
  if (o + 3 > bl)
    return;
  uint32_t end = o + 3 + (((uint32_t)body[o] << 16) | ((uint32_t)body[o + 1] << 8) | body[o + 2]);
  o += 3;
  if (end > bl)
    return;
  uint32_t used = 0;
  while (o + 3 <= end) {
    uint32_t cl = ((uint32_t)body[o] << 16) | ((uint32_t)body[o + 1] << 8) | body[o + 2];
    o += 3;
    if (o + cl > end)
      return;
    if (c->nchain == CERTS_CHAIN_MAX || used + cl > sizeof c->buf) {
      c->verify = X509_E_UNSUPPORTED;
      return;
    }
    memcpy(c->buf + used, body + o, cl);
    int rc = x509_parse(&c->chain[c->nchain], c->buf + used, cl);
    if (rc) {
      c->verify = rc;
      return;
    }
    c->nchain++;
    used += cl;
    o += cl;
    if (tls13) {
      if (o + 2 > end)
        return;
      o += 2 + (((uint32_t)body[o] << 8) | body[o + 1]);
    }
  }
  if (!c->nchain)
    return;
  const struct x509_store *store = roots();
  c->verify = store ? x509_verify_chain(store, c->chain, c->nchain, host, wall_now()) : X509_E_NO_STORE;
}

void certs_check_sig(struct certs *c, uint16_t scheme, const uint8_t *msg, uint32_t msglen, const uint8_t *sig,
                     uint32_t siglen)
{
  if (!c->nchain || x509_verify_tls_sig(&c->chain[0], scheme, msg, msglen, sig, siglen))
    certs_fail(c, X509_E_SIG);
}

void certs_check_cv(struct certs *c, const uint8_t th[32], const uint8_t *body, uint32_t bl)
{
  static const char ctx[] = "TLS 1.3, server CertificateVerify";
  uint8_t signed_content[64 + sizeof ctx + 32];
  memset(signed_content, ' ', 64);
  memcpy(signed_content + 64, ctx, sizeof ctx); /* includes the NUL separator */
  memcpy(signed_content + 64 + sizeof ctx, th, 32);
  uint32_t sl = bl >= 4 ? ((uint32_t)body[2] << 8) | body[3] : 0;
  if (bl < 4 || 4 + sl != bl)
    certs_fail(c, X509_E_SIG);
  else
    certs_check_sig(c, (uint16_t)(((uint16_t)body[0] << 8) | body[1]), signed_content, sizeof signed_content, body + 4,
                    sl);
}

const char *certs_message(const struct certs *c) { return c->verify == X509_OK ? NULL : x509_strerror(c->verify); }
