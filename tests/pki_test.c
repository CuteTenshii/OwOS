/* pki_test.c — RSA, ECDSA and X.509 verification against OpenSSL-made vectors, plus SHA-384. */
#define OWOS_SUITE_TEST
#include "test.h"
#include <stdarg.h>
#include <string.h>

#include "../user/lib/crypto/ed25519.c"
#include "../user/lib/pki/bn.c"
#include "../user/lib/pki/ecdsa.c"
#include "../user/lib/pki/rsa.c"
#include "../user/lib/pki/x509.c"
#include "ecdsa_vectors.h"
#include "rsa_vectors.h"
#include "x509_vectors.h"

static uint32_t unhex(const char *s, uint8_t *out)
{
  uint32_t n = 0;
  for (; s[0] && s[1]; s += 2, n++) {
    int hi = s[0] <= '9' ? s[0] - '0' : s[0] - 'a' + 10;
    int lo = s[1] <= '9' ? s[1] - '0' : s[1] - 'a' + 10;
    out[n] = (uint8_t)(hi << 4 | lo);
  }
  return n;
}

static int hash_of(const char *name) { return name[3] == '2' ? PK_SHA256 : name[3] == '3' ? PK_SHA384 : PK_SHA512; }

static const char *digest_hex(int alg)
{
  return alg == PK_SHA256 ? DIGEST_SHA256 : alg == PK_SHA384 ? DIGEST_SHA384 : DIGEST_SHA512;
}

static int verify(const struct rsa_vec *v, const uint8_t *digest, const uint8_t *sig, uint32_t siglen,
                  const char *scheme)
{
  static const uint8_t e[] = {1, 0, 1};
  uint8_t n[512];
  uint32_t nlen = unhex(v->n, n);
  int alg = hash_of(v->hash);
  return strcmp(scheme, "pss") ? rsa_verify_pkcs1(n, nlen, e, 3, alg, digest, sig, siglen)
                               : rsa_verify_pss(n, nlen, e, 3, alg, digest, pk_hash_len(alg), sig, siglen);
}

static void verifies_openssl_signatures(void)
{
  for (unsigned i = 0; i < sizeof RSA_VECS / sizeof *RSA_VECS; i++) {
    const struct rsa_vec *v = &RSA_VECS[i];
    uint8_t digest[64], sig[512];
    unhex(digest_hex(hash_of(v->hash)), digest);
    uint32_t siglen = unhex(v->sig, sig);
    expect(verify(v, digest, sig, siglen, v->scheme) == 0);
  }
}

static void rejects_tampering(void)
{
  for (unsigned i = 0; i < sizeof RSA_VECS / sizeof *RSA_VECS; i++) {
    const struct rsa_vec *v = &RSA_VECS[i];
    uint8_t digest[64], sig[512];
    unhex(digest_hex(hash_of(v->hash)), digest);
    uint32_t siglen = unhex(v->sig, sig);

    sig[siglen / 2] ^= 1;
    expect(verify(v, digest, sig, siglen, v->scheme) != 0);
    sig[siglen / 2] ^= 1;

    digest[3] ^= 1;
    expect(verify(v, digest, sig, siglen, v->scheme) != 0);
    digest[3] ^= 1;

    expect(verify(v, digest, sig, siglen - 1, v->scheme) != 0);
    expect(verify(v, digest, sig, siglen, strcmp(v->scheme, "pss") ? "pss" : "pkcs1") != 0);
  }
}

static void sha384_known_answer(void)
{
  uint8_t out[48], want[48];
  sha384("abc", 3, out);
  unhex("cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7", want);
  expect(memcmp(out, want, 48) == 0);
}

struct loaded {
  uint8_t pub[97], digest[64], sig[128];
  uint32_t publen, dlen, siglen;
  int curve;
};

static void load(const struct ec_vec *v, struct loaded *l)
{
  l->publen = unhex(v->pub, l->pub);
  l->dlen = unhex(v->digest, l->digest);
  l->siglen = unhex(v->sig, l->sig);
  l->curve = v->p384 ? PK_P384 : PK_P256;
}

static int check(const struct loaded *l)
{
  return ecdsa_verify_der(l->curve, l->pub, l->publen, l->digest, l->dlen, l->sig, l->siglen);
}

static void ec_verifies_openssl_signatures(void)
{
  for (unsigned i = 0; i < sizeof EC_VECS / sizeof *EC_VECS; i++) {
    struct loaded l;
    load(&EC_VECS[i], &l);
    expect(check(&l) == 0);
  }
}

static void ec_rejects_tampering(void)
{
  for (unsigned i = 0; i < sizeof EC_VECS / sizeof *EC_VECS; i++) {
    struct loaded l;
    load(&EC_VECS[i], &l);

    l.sig[l.siglen - 3] ^= 1;
    expect(check(&l) != 0);
    l.sig[l.siglen - 3] ^= 1;

    l.digest[1] ^= 1;
    expect(check(&l) != 0);
    l.digest[1] ^= 1;

    l.pub[40] ^= 1; /* off the curve */
    expect(check(&l) != 0);
    l.pub[40] ^= 1;

    l.curve = !l.curve;
    expect(check(&l) != 0);
    l.curve = !l.curve;

    expect(ecdsa_verify_der(l.curve, l.pub, l.publen, l.digest, l.dlen, l.sig, l.siglen - 1) != 0);
    expect(ecdsa_verify_der(l.curve, l.pub, l.publen - 1, l.digest, l.dlen, l.sig, l.siglen) != 0);
  }
}

/* ------------------------------------------------------------------ X.509 */

struct cert {
  uint8_t der[1600];
  struct x509 x;
};

static void load_cert(struct cert *c, const char *hex)
{
  uint32_t n = unhex(hex, c->der);
  expect(x509_parse(&c->x, c->der, n) == X509_OK);
}

static void load_pem_store(struct x509_store *st, const char *const *pems, int n)
{
  static char buf[16384];
  buf[0] = 0;
  for (int i = 0; i < n; i++)
    strcat(buf, pems[i]);
  x509_store_load_pem(st, (const uint8_t *)buf, (uint32_t)strlen(buf));
}

/* Store holding one DER certificate, via a PEM round trip would need base64; build it directly. */
static void store_of(struct x509_store *st, struct cert **roots, int n)
{
  memset(st, 0, sizeof *st);
  st->certs = calloc((size_t)n, sizeof *st->certs);
  for (int i = 0; i < n; i++)
    st->certs[i] = roots[i]->x;
  st->count = (uint32_t)n;
}

static void parses_fields(void)
{
  struct cert leaf, root;
  load_cert(&leaf, CERT_LEAFA1);
  load_cert(&root, CERT_ROOTA);
  expect(leaf.x.key_type == X509_KEY_RSA && leaf.x.keylen == 256);
  expect(leaf.x.sig_alg == X509_SIG_ECDSA_SHA256);
  expect(!leaf.x.is_ca && root.x.is_ca && root.x.pathlen == 1);
  expect(leaf.x.eku_server_auth && leaf.x.has_ku && !leaf.x.ku_cert_sign);
  expect(root.x.ku_cert_sign);
  expect(leaf.x.not_after > leaf.x.not_before);
  struct cert ed;
  load_cert(&ed, CERT_LEAFA3);
  expect(ed.x.key_type == X509_KEY_ED25519 && ed.x.keylen == 32);
  struct cert ec;
  load_cert(&ec, CERT_LEAFA2);
  expect(ec.x.key_type == X509_KEY_P384 && ec.x.keylen == 97);
  for (uint32_t cut = 0; cut < leaf.x.derlen; cut += 7) {
    struct x509 t;
    expect(x509_parse(&t, leaf.der, cut) != X509_OK);
  }
}

static struct cert C_ROOTA, C_INTA, C_LEAFA1, C_LEAFA2, C_LEAFA3, C_ROOTB, C_INTB, C_LEAFB, C_CHILD, C_INTA2, C_LEAFP,
    C_EKU, C_ROOTX, C_LEAFX, C_SHORT;

static void load_all(void)
{
  load_cert(&C_ROOTA, CERT_ROOTA);
  load_cert(&C_INTA, CERT_INTA);
  load_cert(&C_LEAFA1, CERT_LEAFA1);
  load_cert(&C_LEAFA2, CERT_LEAFA2);
  load_cert(&C_LEAFA3, CERT_LEAFA3);
  load_cert(&C_ROOTB, CERT_ROOTB);
  load_cert(&C_INTB, CERT_INTB);
  load_cert(&C_LEAFB, CERT_LEAFB);
  load_cert(&C_CHILD, CERT_CHILDOFLEAF);
  load_cert(&C_INTA2, CERT_INTA2);
  load_cert(&C_LEAFP, CERT_LEAFP);
  load_cert(&C_EKU, CERT_LEAFEKU);
  load_cert(&C_ROOTX, CERT_ROOTX);
  load_cert(&C_LEAFX, CERT_LEAFX);
  load_cert(&C_SHORT, CERT_LEAFSHORT);
}

static int chain(struct x509_store *st, const char *host, int64_t now, int n, ...)
{
  struct x509 list[8];
  va_list ap;
  va_start(ap, n);
  for (int i = 0; i < n; i++)
    list[i] = va_arg(ap, struct cert *)->x;
  va_end(ap);
  return x509_verify_chain(st, list, (uint32_t)n, host, now);
}

static void verifies_chains(void)
{
  load_all();
  struct cert *ra[] = {&C_ROOTA}, *rb[] = {&C_ROOTB}, *rx[] = {&C_ROOTX};
  struct x509_store A, B, X;
  store_of(&A, ra, 1);
  store_of(&B, rb, 1);
  store_of(&X, rx, 1);

  expect(chain(&A, "example.test", 0, 2, &C_LEAFA1, &C_INTA) == X509_OK);
  expect(chain(&A, "EXAMPLE.test", 0, 2, &C_LEAFA1, &C_INTA) == X509_OK);
  expect(chain(&A, "foo.wild.test", 0, 2, &C_LEAFA1, &C_INTA) == X509_OK);
  expect(chain(&A, "wild.test", 0, 2, &C_LEAFA1, &C_INTA) == X509_E_HOSTNAME);
  expect(chain(&A, "a.b.wild.test", 0, 2, &C_LEAFA1, &C_INTA) == X509_E_HOSTNAME);
  expect(chain(&A, "other.test", 0, 2, &C_LEAFA1, &C_INTA) == X509_E_HOSTNAME);
  expect(chain(&A, "10.0.0.1", 0, 2, &C_LEAFA1, &C_INTA) == X509_OK);
  expect(chain(&A, "10.0.0.2", 0, 2, &C_LEAFA1, &C_INTA) == X509_E_HOSTNAME);
  expect(chain(&A, "ec.example.test", 0, 2, &C_LEAFA2, &C_INTA) == X509_OK);
  expect(chain(&A, "ed.example.test", 0, 2, &C_LEAFA3, &C_INTA) == X509_OK);
  expect(chain(&A, "example.test", 0, 3, &C_LEAFA1, &C_INTA, &C_ROOTA) == X509_OK);
  expect(chain(&A, "example.test", 0, 3, &C_INTA, &C_LEAFA1, &C_INTA) != X509_OK);
  expect(chain(&B, "b.example.test", 0, 2, &C_LEAFB, &C_INTB) == X509_OK);

  expect(chain(&A, "example.test", 0, 1, &C_LEAFA1) == X509_E_UNTRUSTED);
  expect(chain(&X, "example.test", 0, 2, &C_LEAFA1, &C_INTA) == X509_E_UNTRUSTED);
  expect(chain(&A, "x.test", 0, 1, &C_LEAFX) == X509_E_UNTRUSTED);
  expect(chain(&B, "example.test", 0, 2, &C_LEAFA1, &C_INTA) == X509_E_UNTRUSTED);
  expect(chain(0, "example.test", 0, 2, &C_LEAFA1, &C_INTA) == X509_E_UNTRUSTED);

  expect(chain(&A, "child.test", 0, 3, &C_CHILD, &C_LEAFA1, &C_INTA) == X509_E_NOT_CA);
  expect(chain(&A, "deep.example.test", 0, 3, &C_LEAFP, &C_INTA2, &C_INTA) == X509_E_NOT_CA);
  expect(chain(&A, "eku.test", 0, 2, &C_EKU, &C_INTA) == X509_E_PURPOSE);

  struct cert bad = C_LEAFA1;
  bad.der[bad.x.derlen - 20] ^= 1; /* inside the signature */
  expect(x509_parse(&bad.x, bad.der, bad.x.derlen) == X509_OK);
  expect(chain(&A, "example.test", 0, 2, &bad, &C_INTA) == X509_E_SIG);

  int64_t t0 = PKI_NOT_BEFORE;
  expect(chain(&A, "short.test", t0 + 86400, 2, &C_SHORT, &C_INTA) == X509_OK);
  expect(chain(&A, "short.test", t0 + 10 * 86400, 2, &C_SHORT, &C_INTA) == X509_E_EXPIRED);
  expect(chain(&A, "short.test", t0 - 10 * 86400, 2, &C_SHORT, &C_INTA) == X509_E_NOT_YET);
  free(A.certs);
  free(B.certs);
  free(X.certs);
}

static void checks_real_roots(void)
{
  for (unsigned i = 0; i < sizeof REAL_ROOTS / sizeof *REAL_ROOTS; i++) {
    struct x509_store st;
    expect(x509_store_load_pem(&st, (const uint8_t *)REAL_ROOTS[i], (uint32_t)strlen(REAL_ROOTS[i])) == 1);
    expect(x509_check_signature(&st.certs[0], &st.certs[0]) == X509_OK);
    expect(st.certs[0].is_ca && st.certs[0].ku_cert_sign);
    x509_store_free(&st);
  }
  struct x509_store st;
  expect(x509_store_load_pem(&st, (const uint8_t *)SHA1_ROOT, (uint32_t)strlen(SHA1_ROOT)) == 1);
  expect(x509_check_signature(&st.certs[0], &st.certs[0]) == X509_E_UNSUPPORTED);
  x509_store_free(&st);

  struct x509_store all;
  load_pem_store(&all, REAL_ROOTS, (int)(sizeof REAL_ROOTS / sizeof *REAL_ROOTS));
  expect(all.count == sizeof REAL_ROOTS / sizeof *REAL_ROOTS);
  x509_store_free(&all);
  expect(x509_store_load_pem(&all, (const uint8_t *)"not a bundle", 12) == 0);
  x509_store_free(&all);
}

static void verifies_tls_signatures(void)
{
  load_all();
  static const uint8_t msg[] = "tls signed content";
  uint8_t sig[512];
  uint32_t n = unhex(SIG_RSA_PSS256, sig);
  expect(x509_verify_tls_sig(&C_LEAFA1.x, 0x0804, msg, sizeof msg - 1, sig, n) == X509_OK);
  expect(x509_verify_tls_sig(&C_LEAFA1.x, 0x0401, msg, sizeof msg - 1, sig, n) != X509_OK);
  expect(x509_verify_tls_sig(&C_LEAFA1.x, 0x0804, msg, sizeof msg - 2, sig, n) != X509_OK);
  n = unhex(SIG_RSA_PKCS256, sig);
  expect(x509_verify_tls_sig(&C_LEAFA1.x, 0x0401, msg, sizeof msg - 1, sig, n) == X509_OK);
  n = unhex(SIG_EC384, sig);
  expect(x509_verify_tls_sig(&C_LEAFA2.x, 0x0503, msg, sizeof msg - 1, sig, n) == X509_OK);
  expect(x509_verify_tls_sig(&C_LEAFA2.x, 0x0403, msg, sizeof msg - 1, sig, n) != X509_OK);
  expect(x509_verify_tls_sig(&C_LEAFA1.x, 0x0503, msg, sizeof msg - 1, sig, n) != X509_OK);
  n = unhex(SIG_ED, sig);
  expect(x509_verify_tls_sig(&C_LEAFA3.x, 0x0807, msg, sizeof msg - 1, sig, n) == X509_OK);
  sig[5] ^= 1;
  expect(x509_verify_tls_sig(&C_LEAFA3.x, 0x0807, msg, sizeof msg - 1, sig, n) != X509_OK);
  expect(x509_verify_tls_sig(&C_LEAFA3.x, 0x0001, msg, sizeof msg - 1, sig, n) == X509_E_UNSUPPORTED);
}

static void verifies_rsa_pss_certificates(void)
{
  static struct cert root, l384, l20;
  load_cert(&root, CERT_ROOTPSS);
  load_cert(&l384, CERT_LEAFPSS384);
  load_cert(&l20, CERT_LEAFPSS20);
  expect(root.x.sig_alg == X509_SIG_RSA_PSS && root.x.pss_hash == PK_SHA256 && root.x.pss_saltlen == 32);
  expect(l384.x.sig_alg == X509_SIG_RSA_PSS && l384.x.pss_hash == PK_SHA384 && l384.x.pss_saltlen == 48);
  expect(l20.x.pss_saltlen == 20);
  expect(x509_check_signature(&root.x, &root.x) == X509_OK);
  expect(x509_check_signature(&l384.x, &root.x) == X509_OK);
  expect(x509_check_signature(&l20.x, &root.x) == X509_OK);
  expect(x509_check_signature(&l20.x, &l384.x) != X509_OK);

  struct cert bad = l384;
  bad.der[bad.x.derlen - 10] ^= 1;
  expect(x509_parse(&bad.x, bad.der, bad.x.derlen) == X509_OK);
  expect(x509_check_signature(&bad.x, &root.x) == X509_E_SIG);

  struct cert *roots[] = {&root};
  struct x509_store st;
  store_of(&st, roots, 1);
  expect(chain(&st, "pss.test", 0, 1, &l384) == X509_OK);
  expect(chain(&st, "pss20.test", 0, 1, &l20) == X509_OK);
  free(st.certs);
}

static struct cert N_ROOT, N_OK, N_BAD, N_BADSUB, N_OTHER, N_IP, N_WILD, N2_ROOT, N2_SUB, N2_APEX, N2_WILD, N3_ROOT,
    N3_INT, N3_OK, N3_BAD;

static void enforces_name_constraints(void)
{
  load_cert(&N_ROOT, CERT_ROOTNC);
  load_cert(&N_OK, CERT_NCOK);
  load_cert(&N_BAD, CERT_NCBAD);
  load_cert(&N_BADSUB, CERT_NCBADSUB);
  load_cert(&N_OTHER, CERT_NCOTHER);
  load_cert(&N_IP, CERT_NCIP);
  load_cert(&N_WILD, CERT_NCWILD);
  load_cert(&N2_ROOT, CERT_ROOTNC2);
  load_cert(&N2_SUB, CERT_NC2SUB);
  load_cert(&N2_APEX, CERT_NC2APEX);
  load_cert(&N2_WILD, CERT_NC2WILD);
  load_cert(&N3_ROOT, CERT_ROOTN3);
  load_cert(&N3_INT, CERT_INTNC);
  load_cert(&N3_OK, CERT_INTOK);
  load_cert(&N3_BAD, CERT_INTBAD);
  expect(N_ROOT.x.nc && !N_ROOT.x.unsupported_critical);

  struct cert *r1[] = {&N_ROOT}, *r2[] = {&N2_ROOT}, *r3[] = {&N3_ROOT};
  struct x509_store s1, s2, s3;
  store_of(&s1, r1, 1);
  store_of(&s2, r2, 1);
  store_of(&s3, r3, 1);

  expect(chain(&s1, "ok.permitted.test", 0, 1, &N_OK) == X509_OK);
  expect(chain(&s1, "10.1.2.3", 0, 1, &N_OK) == X509_OK);
  expect(chain(&s1, "bad.permitted.test", 0, 1, &N_BAD) == X509_E_NAME_CONSTRAINT);
  expect(chain(&s1, "sub.bad.permitted.test", 0, 1, &N_BADSUB) == X509_E_NAME_CONSTRAINT);
  expect(chain(&s1, "other.test", 0, 1, &N_OTHER) == X509_E_NAME_CONSTRAINT);
  expect(chain(&s1, "ok.permitted.test", 0, 1, &N_IP) == X509_E_NAME_CONSTRAINT);
  expect(chain(&s1, "x.permitted.test", 0, 1, &N_WILD) == X509_E_NAME_CONSTRAINT); /* could cover the excluded name */

  expect(chain(&s2, "a.example.org", 0, 1, &N2_SUB) == X509_OK); /* an IP name is unconstrained here */
  expect(chain(&s2, "example.org", 0, 1, &N2_APEX) == X509_E_NAME_CONSTRAINT);
  expect(chain(&s2, "x.example.org", 0, 1, &N2_WILD) == X509_OK);

  expect(chain(&s3, "a.int.test", 0, 2, &N3_OK, &N3_INT) == X509_OK);
  expect(chain(&s3, "x.test", 0, 2, &N3_BAD, &N3_INT) == X509_E_NAME_CONSTRAINT);
  free(s1.certs);
  free(s2.certs);
  free(s3.certs);
}

int main(void)
{
  test("verifies OpenSSL RSA signatures", verifies_openssl_signatures);
  test("rejects tampered signatures and digests", rejects_tampering);
  test("SHA-384", sha384_known_answer);
  test("verifies OpenSSL ECDSA signatures", ec_verifies_openssl_signatures);
  test("rejects tampered ECDSA input", ec_rejects_tampering);
  test("parses X.509 fields", parses_fields);
  test("verifies certificate chains", verifies_chains);
  test("checks real root signatures", checks_real_roots);
  test("verifies TLS handshake signatures", verifies_tls_signatures);
  test("verifies RSASSA-PSS certificates", verifies_rsa_pss_certificates);
  test("enforces name constraints", enforces_name_constraints);
  return 0;
}
