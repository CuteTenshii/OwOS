/* Host test for the zstd codec (user/lib/zstd): XXH64 vectors, self round trips, and interop in both directions
 * against the reference `zstd` CLI when it is installed. */
#define OWOS_SUITE_TEST
#include "test.h"
#include "zstd.h"
#include "zstd_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char dir[512];
static int have_cli;

static int write_file(const char *path, const unsigned char *buf, int len)
{
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  int ok = len == 0 || fwrite(buf, 1, (size_t)len, f) == (size_t)len;
  fclose(f);
  return ok ? 0 : -1;
}

static unsigned char *read_file(const char *path, int *len)
{
  FILE *f = fopen(path, "rb");
  *len = -1;
  if (!f)
    return 0;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc(n ? (size_t)n : 1);
  *len = (int)fread(b, 1, (size_t)n, f);
  fclose(f);
  return b;
}

static unsigned char *gen_text(int n)
{
  static const char *const words[] = {"the ",  "quick ", "brown ", "fox ",  "jumps ", "over ", "a ",
                                      "lazy ", "dog ",   "and ",   "then ", "runs ",  "away "};
  unsigned char *b = malloc(n ? (size_t)n : 1);
  unsigned s = 12345;
  int p = 0;
  while (p < n) {
    s = s * 1103515245u + 12345u;
    const char *w = words[(s >> 16) % 13];
    for (int i = 0; w[i] && p < n; i++)
      b[p++] = (unsigned char)w[i];
  }
  return b;
}

static unsigned char *gen_binary(int n)
{
  unsigned char *b = malloc(n ? (size_t)n : 1);
  unsigned s = 99;
  for (int i = 0; i < n; i++) {
    s = s * 1664525u + 1013904223u;
    b[i] = (unsigned char)(s >> 24);
  }
  return b;
}

/* Byte runs of 1..40: long overlapping matches, which reach the upper match-length codes. */
static unsigned char *gen_runs(int n)
{
  unsigned char *b = malloc(n ? (size_t)n : 1);
  unsigned s = 7;
  int p = 0;
  while (p < n) {
    s = s * 1103515245u + 12345u;
    int run = 1 + (int)((s >> 8) % 40);
    unsigned char v = (unsigned char)(s >> 20);
    for (int i = 0; i < run && p < n; i++)
      b[p++] = v;
  }
  return b;
}

static void expect_decodes_to(const unsigned char *comp, int clen, const unsigned char *want, int n)
{
  int dlen = -1;
  unsigned char *dec = zstd_decompress(comp, clen, &dlen);
  expect(dec != 0);
  expect_eq(dlen, n);
  expect(dec && dlen == n && (n == 0 || memcmp(dec, want, (size_t)n) == 0));
  free(dec);
}

static void roundtrip(const unsigned char *in, int n, int level)
{
  int clen = 0;
  unsigned char *comp = zstd_compress(in, n, &clen, level);
  expect(comp != 0);
  if (!comp)
    return;
  expect_decodes_to(comp, clen, in, n);

  if (have_cli) {
    char src[600], out[600], cmd[1400];
    snprintf(src, sizeof src, "%s/mine.zst", dir);
    snprintf(out, sizeof out, "%s/mine.out", dir);
    unlink(out);
    expect_eq(write_file(src, comp, clen), 0);
    snprintf(cmd, sizeof cmd, "zstd -d -f -q '%s' -o '%s' 2>/dev/null", src, out);
    expect_eq(system(cmd), 0);
    int rn = 0;
    unsigned char *ref = read_file(out, &rn);
    expect_eq(rn, n);
    expect(ref && rn == n && (n == 0 || memcmp(ref, in, (size_t)n) == 0));
    free(ref);
  }
  free(comp);
}

static void reference_to_mine(const unsigned char *in, int n, const char *flags)
{
  char src[600], out[600], cmd[1400];
  snprintf(src, sizeof src, "%s/in.bin", dir);
  snprintf(out, sizeof out, "%s/ref.zst", dir);
  expect_eq(write_file(src, in, n), 0);
  snprintf(cmd, sizeof cmd, "zstd %s -f -q '%s' -o '%s' 2>/dev/null", flags, src, out);
  expect_eq(system(cmd), 0);
  int cn = 0;
  unsigned char *comp = read_file(out, &cn);
  expect(comp && cn > 0);
  if (comp && cn > 0)
    expect_decodes_to(comp, cn, in, n);
  free(comp);
}

static void check_xxh64(void)
{
  const char *fox = "The quick brown fox jumps over the lazy dog";
  expect(xxh64("", 0, 0) == 0xEF46DB3751D8E999ull);
  expect(xxh64("a", 1, 0) == 0xD24EC4F1A98C6E5Bull);
  expect(xxh64(fox, (u32)strlen(fox), 0) == 0x0B242D361FDA71BCull);
}

static void check_generated_inputs(void)
{
  static unsigned char *(*const gens[])(int) = {gen_text, gen_binary, gen_runs};
  static const int sizes[] = {0, 1, 2, 7, 31, 63, 100, 255, 256, 1000, 4096, 65535, 70000, 200000, 500000};
  static const int levels[] = {1, 2, 3, 12};
  for (size_t g = 0; g < sizeof gens / sizeof gens[0]; g++) {
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
      unsigned char *in = gens[g](sizes[s]);
      for (size_t l = 0; l < sizeof levels / sizeof levels[0]; l++)
        roundtrip(in, sizes[s], levels[l]);
      if (have_cli) {
        reference_to_mine(in, sizes[s], "-1");
        reference_to_mine(in, sizes[s], "-9");
        reference_to_mine(in, sizes[s], "-19 --no-check");
      }
      free(in);
    }
  }
}

static void check_multi_block(void)
{
  int n = 400000;
  unsigned char *in = malloc((size_t)n);
  for (int i = 0; i < n; i++)
    in[i] = (unsigned char)('A' + i % 26);
  roundtrip(in, n, 6);
  if (have_cli)
    reference_to_mine(in, n, "-3");
  free(in);
}

/* One compressed block declaring a single sequence whose literal-length table is RLE of symbol 0xFF, past MAX_LL;
 * the decoder must reject it rather than index the length tables with it. */
static void check_out_of_range_symbol(void)
{
  static const unsigned char frame[] = {
      0x28, 0xB5, 0x2F, 0xFD, /* magic */
      0x20, 0x01,             /* single segment, content size 1 */
      0x35, 0x00, 0x00,       /* last block, compressed, 6 bytes */
      0x00,                   /* raw literals, length 0 */
      0x01,                   /* one sequence */
      0x40,                   /* LL = RLE, OF and ML predefined */
      0xFF,                   /* the RLE symbol */
      0x00, 0x80,             /* bitstream with its end marker */
  };
  int dlen = 0;
  unsigned char *dec = zstd_decompress(frame, (int)sizeof frame, &dlen);
  expect(dec == 0);
  free(dec);
}

/* Flipping any byte of a valid frame must fail cleanly or decode, never crash; the content checksum catches most. */
static void check_corrupt_frames(void)
{
  int n = 5000, clen = 0;
  unsigned char *in = gen_runs(n);
  unsigned char *comp = zstd_compress(in, n, &clen, 3);
  expect(comp != 0);
  unsigned char *bad = malloc((size_t)clen);
  int dlen = 0;
  for (int i = 0; comp && i < clen; i++) {
    memcpy(bad, comp, (size_t)clen);
    bad[i] ^= 0x5A;
    free(zstd_decompress(bad, clen, &dlen));
  }
  /* Empty input is a valid empty stream, so truncation starts at one byte. */
  for (int cut = 1; comp && cut < clen; cut++)
    expect(zstd_decompress(comp, cut, &dlen) == 0);
  free(bad);
  free(comp);
  free(in);
}

int main(void)
{
  const char *tmp = getenv("TMPDIR");
  snprintf(dir, sizeof dir, "%s/owos-zstd-XXXXXX", tmp && *tmp ? tmp : "/tmp");
  expect(mkdtemp(dir) != 0);
  have_cli = system("zstd --version >/dev/null 2>&1") == 0;
  if (!have_cli)
    printf("zstd: reference CLI not installed, skipping interop checks\n");

  test("xxh64 vectors", check_xxh64);
  test("generated inputs", check_generated_inputs);
  test("multi-block input", check_multi_block);
  test("out-of-range sequence symbol", check_out_of_range_symbol);
  test("corrupt frames", check_corrupt_frames);

  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  (void)!system(cmd);
  return 0;
}
