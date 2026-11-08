/* ech_test.c — ECHConfig selection and the ClientHelloInner / outer extension encodings. */
#define OWOS_SUITE_TEST
#include "test.h"
#include <string.h>

#include "../user/lib/tls/ech.c"

/* ECHConfigList from `openssl ech`: id 0x14, DHKEM(X25519), HKDF-SHA256/AES-128-GCM,
 * max_name_len 32, public_name "public.example". */
static const uint8_t LIST[] = {0x00, 0x41, 0xfe, 0x0d, 0x00, 0x3d, 0x14, 0x00, 0x20, 0x00, 0x20, 0x52, 0xbe, 0x4b,
                               0x3e, 0x2b, 0x38, 0xf0, 0x2e, 0xf7, 0x34, 0xf2, 0xd6, 0xb9, 0x4e, 0xed, 0x03, 0x86,
                               0x82, 0x9f, 0x2f, 0xd1, 0xf5, 0xc9, 0x8a, 0x76, 0x03, 0xe4, 0xd8, 0x24, 0xb3, 0xfb,
                               0x0f, 0x00, 0x04, 0x00, 0x01, 0x00, 0x01, 0x20, 0x0e, 'p',  'u',  'b',  'l',  'i',
                               'c',  '.',  'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x00, 0x00};

static void picks_config(void)
{
  struct ech_cfg cfg;
  expect(ech_pick_config(LIST, sizeof LIST, &cfg) == 1);
  expect(cfg.config_id == 0x14);
  expect(cfg.max_name_len == 32);
  expect(cfg.pub[0] == 0x52 && cfg.pub[31] == 0x0f);
  expect(strcmp(cfg.public_name, "public.example") == 0);
  expect(cfg.raw == LIST + 2 && cfg.rawlen == sizeof LIST - 2);
}

static void rejects_unusable_configs(void)
{
  uint8_t b[sizeof LIST];
  struct ech_cfg cfg;

  memcpy(b, LIST, sizeof b);
  b[9] = 0x10; /* KEM: P-256 */
  expect(ech_pick_config(b, sizeof b, &cfg) == 0);

  memcpy(b, LIST, sizeof b);
  b[46] = 0x03; /* AEAD: ChaCha20-Poly1305 only */
  expect(ech_pick_config(b, sizeof b, &cfg) == 0);

  memcpy(b, LIST, sizeof b);
  b[2] = 0xff; /* version */
  expect(ech_pick_config(b, sizeof b, &cfg) == 0);

  /* a mandatory extension (high bit set) makes the config unusable */
  uint8_t m[sizeof LIST + 4];
  memcpy(m, LIST, sizeof LIST - 2);
  m[sizeof LIST - 2] = 0x00;
  m[sizeof LIST - 1] = 0x04;
  m[sizeof LIST] = 0x80;
  m[sizeof LIST + 1] = 0x01;
  m[sizeof LIST + 2] = 0x00;
  m[sizeof LIST + 3] = 0x00;
  m[1] += 4;
  m[5] += 4;
  expect(ech_pick_config(m, sizeof m, &cfg) == 0);
  m[sizeof LIST] = 0x00; /* same extension, optional */
  expect(ech_pick_config(m, sizeof m, &cfg) == 1);

  for (uint32_t cut = 0; cut < sizeof LIST; cut++)
    expect(ech_pick_config(LIST, cut, &cfg) == 0);
}

static void encodes_inner_padded(void)
{
  struct ech_cfg cfg;
  ech_pick_config(LIST, sizeof LIST, &cfg);

  uint8_t ch[4 + 2 + 32 + 33 + 10] = {1, 0, 0, 77, 3, 3};
  for (int i = 0; i < 32; i++)
    ch[6 + i] = (uint8_t)(0xA0 + i);
  ch[38] = 32; /* legacy_session_id */
  for (int i = 0; i < 32; i++)
    ch[39 + i] = 0xEE;
  memcpy(ch + 71, "TAILTAILTA", 10);

  uint8_t out[256];
  memset(out, 0xCC, sizeof out);
  uint32_t n = ech_encode_inner(ch, sizeof ch, 14, &cfg, out, sizeof out);
  expect(n == 64); /* 45 encoded + padding up to a multiple of 32 */
  expect(out[0] == 3 && out[1] == 3 && out[2] == 0xA0 && out[33] == 0xBF);
  expect(out[34] == 0);
  expect(memcmp(out + 35, "TAILTAILTA", 10) == 0);
  for (uint32_t i = 45; i < n; i++)
    expect(out[i] == 0);

  /* a longer name needs less name padding but is still rounded to 32 */
  n = ech_encode_inner(ch, sizeof ch, 32, &cfg, out, sizeof out);
  expect(n % 32 == 0 && n >= 45);

  expect(ech_encode_inner(ch, sizeof ch, 14, &cfg, out, 63) == 0);
  ch[38] = 200; /* session id longer than the message */
  expect(ech_encode_inner(ch, sizeof ch, 14, &cfg, out, sizeof out) == 0);
}

static void outer_extension_layout(void)
{
  struct ech_cfg cfg;
  ech_pick_config(LIST, sizeof LIST, &cfg);
  uint8_t enc[32], out[200];
  for (int i = 0; i < 32; i++)
    enc[i] = (uint8_t)i;
  memset(out, 0xCC, sizeof out);

  uint32_t n = ech_outer_ext(out, &cfg, enc, 100);
  expect(n == 4 + 1 + 4 + 1 + 2 + 32 + 2 + 100);
  expect(out[0] == 0xfe && out[1] == 0x0d);
  expect(((out[2] << 8) | out[3]) == (int)n - 4);
  expect(out[4] == 0); /* outer */
  expect(out[5] == 0 && out[6] == 1 && out[7] == 0 && out[8] == 1);
  expect(out[9] == 0x14);
  expect(out[10] == 0 && out[11] == 32 && memcmp(out + 12, enc, 32) == 0);
  expect(out[44] == 0 && out[45] == 100);
  for (uint32_t i = 46; i < n; i++)
    expect(out[i] == 0);
}

int main(void)
{
  test("picks a usable ECHConfig", picks_config);
  test("rejects unusable configs and truncations", rejects_unusable_configs);
  test("encodes ClientHelloInner with padding", encodes_inner_padded);
  test("outer extension layout", outer_extension_layout);
  return 0;
}
