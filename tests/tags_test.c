/* tags_test.c, host-side unit test for user/lib/media/tags.c: ID3v2/v1, MP4 ilst and WAV INFO metadata. */
#define OWOS_SUITE_TEST
#include "media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH "/tmp/owos_tags_test.bin"

static unsigned char B[4096];
static unsigned N;

static void put(const void *p, unsigned n)
{
  memcpy(B + N, p, n);
  N += n;
}
static void u8(unsigned v) { B[N++] = (unsigned char)v; }
static void be32(unsigned v)
{
  u8(v >> 24);
  u8(v >> 16);
  u8(v >> 8);
  u8(v);
}
static void le32(unsigned v)
{
  u8(v);
  u8(v >> 8);
  u8(v >> 16);
  u8(v >> 24);
}
static void ss32(unsigned v)
{
  u8(v >> 21 & 0x7f);
  u8(v >> 14 & 0x7f);
  u8(v >> 7 & 0x7f);
  u8(v & 0x7f);
}

static int read_file(struct media_tags *t)
{
  FILE *f = fopen(PATH, "wb");
  fwrite(B, 1, N, f);
  fclose(f);
  int r = media_tags_read(PATH, t);
  remove(PATH);
  return r;
}

/* ID3v2 text frame: id, encoding byte, payload */
static void id3_frame(unsigned ver, const char *id, int enc, const void *s, unsigned n)
{
  if (ver == 2) {
    put(id, 3);
    u8(0), u8((n + 1) >> 8), u8(n + 1);
  } else {
    put(id, 4);
    if (ver == 4)
      ss32(n + 1);
    else
      be32(n + 1);
    u8(0), u8(0);
  }
  u8(enc);
  put(s, n);
}

static void id3_begin(unsigned ver, unsigned body)
{
  N = 0;
  put("ID3", 3);
  u8(ver), u8(0), u8(0);
  ss32(body);
}

static void test_id3v23_utf16_and_latin1(void)
{
  struct media_tags t;
  const unsigned char title[] = {0xFF, 0xFE, 'H', 0, 0xE9, 0, 'y', 0};
  id3_begin(3, 0);
  id3_frame(3, "TIT2", 1, title, sizeof title);
  id3_frame(3, "TPE1", 0, "Caf\xE9 Band", 9);
  id3_frame(3, "TXXX", 0, "ignored", 7);
  unsigned body = N - 10;
  B[6] = body >> 21 & 0x7f, B[7] = body >> 14 & 0x7f, B[8] = body >> 7 & 0x7f, B[9] = body & 0x7f;
  expect_eq(read_file(&t), 1);
  expect_string(t.title, "H\xC3\xA9y");
  expect_string(t.artist, "Caf\xC3\xA9 Band");
  expect_string(t.album, "");
}

static void test_id3v24_utf8(void)
{
  struct media_tags t;
  id3_begin(4, 0);
  id3_frame(4, "TALB", 3, "Alb\xE2\x82\xAC", 6);
  id3_frame(4, "TIT2", 3, "Song", 4);
  unsigned body = N - 10;
  B[6] = body >> 21 & 0x7f, B[7] = body >> 14 & 0x7f, B[8] = body >> 7 & 0x7f, B[9] = body & 0x7f;
  expect_eq(read_file(&t), 1);
  expect_string(t.title, "Song");
  expect_string(t.album, "Alb\xE2\x82\xAC");
}

static void test_id3v22(void)
{
  struct media_tags t;
  id3_begin(2, 0);
  id3_frame(2, "TT2", 0, "Old", 3);
  id3_frame(2, "TP1", 0, "Timer", 5);
  unsigned body = N - 10;
  B[9] = body & 0x7f, B[8] = body >> 7 & 0x7f;
  expect_eq(read_file(&t), 1);
  expect_string(t.title, "Old");
  expect_string(t.artist, "Timer");
}

static void test_id3v1_fallback(void)
{
  struct media_tags t;
  N = 3900;
  memset(B, 0, 3900);
  put("TAG", 3);
  char f[30];
  memset(f, ' ', 30);
  memcpy(f, "Tiny", 4);
  put(f, 30);
  memset(f, ' ', 30);
  memcpy(f, "Artist", 6);
  put(f, 30);
  memset(f, 0, 30);
  put(f, 30);
  while (N < 3900 + 128)
    B[N++] = 0;
  expect_eq(read_file(&t), 1);
  expect_string(t.title, "Tiny");
  expect_string(t.artist, "Artist");
  expect_string(t.album, "");
}

static void box_begin(unsigned *at, const char *type)
{
  *at = N;
  be32(0);
  put(type, 4);
}
static void box_end(unsigned at)
{
  unsigned n = N - at;
  B[at] = n >> 24, B[at + 1] = n >> 16, B[at + 2] = n >> 8, B[at + 3] = n;
}

static void ilst_item(const char *type, const char *text)
{
  unsigned item, data;
  box_begin(&item, type);
  box_begin(&data, "data");
  be32(1), be32(0);
  put(text, strlen(text));
  box_end(data);
  box_end(item);
}

static void test_mp4_ilst(void)
{
  struct media_tags t;
  unsigned ftyp, moov, trak, udta, meta, ilst;
  N = 0;
  box_begin(&ftyp, "ftyp");
  put("M4A \0\0\0\0", 8);
  box_end(ftyp);
  box_begin(&moov, "moov");
  box_begin(&trak, "trak");
  box_end(trak);
  box_begin(&udta, "udta");
  box_begin(&meta, "meta");
  be32(0);
  box_begin(&ilst, "ilst");
  ilst_item("\xA9"
            "nam",
            "Track One");
  ilst_item("\xA9"
            "ART",
            "The Band");
  ilst_item("\xA9"
            "alb",
            "The Album");
  box_end(ilst);
  box_end(meta);
  box_end(udta);
  box_end(moov);
  expect_eq(read_file(&t), 1);
  expect_string(t.title, "Track One");
  expect_string(t.artist, "The Band");
  expect_string(t.album, "The Album");
}

static void info_chunk(const char *id, const char *text)
{
  unsigned n = strlen(text) + 1;
  put(id, 4);
  le32(n);
  put(text, n);
  if (n & 1)
    u8(0);
}

static void test_wav_info(void)
{
  struct media_tags t;
  N = 0;
  put("RIFF", 4);
  le32(0);
  put("WAVE", 4);
  put("fmt ", 4);
  le32(16);
  memset(B + N, 0, 16);
  N += 16;
  unsigned list = N;
  put("LIST", 4);
  le32(0);
  put("INFO", 4);
  info_chunk("INAM", "Wave Title");
  info_chunk("IART", "Wave Artist");
  info_chunk("IPRD", "Wave Album");
  unsigned n = N - list - 8;
  B[list + 4] = n, B[list + 5] = n >> 8;
  put("data", 4);
  le32(0);
  expect_eq(read_file(&t), 1);
  expect_string(t.title, "Wave Title");
  expect_string(t.artist, "Wave Artist");
  expect_string(t.album, "Wave Album");
}

static void vc_entry(const char *s)
{
  le32(strlen(s));
  put(s, strlen(s));
}

static void b64(const unsigned char *d, unsigned n, char *out)
{
  static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned o = 0;
  for (unsigned i = 0; i < n; i += 3) {
    unsigned v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    out[o++] = t[v >> 18];
    out[o++] = t[v >> 12 & 63];
    out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
    out[o++] = i + 2 < n ? t[v & 63] : '=';
  }
  out[o] = 0;
}

/* FLAC PICTURE block body */
static void flac_picture(unsigned type, const char *mime, const void *img, unsigned n)
{
  be32(type);
  be32(strlen(mime));
  put(mime, strlen(mime));
  be32(0);
  be32(0), be32(0), be32(24), be32(0);
  be32(n);
  put(img, n);
}

static void test_id3_apic_prefers_front_cover(void)
{
  struct media_tags t;
  unsigned len;
  id3_begin(3, 0);
  /* APIC: enc 0, "image/jpeg\0", type 0 (other), desc "\0", data */
  put("APIC", 4);
  be32(1 + 11 + 1 + 1 + 5);
  u8(0), u8(0);
  u8(0), put("image/jpeg", 11), u8(0), u8(0), put("OTHER", 5);
  put("APIC", 4);
  be32(1 + 10 + 1 + 3 + 5);
  u8(0), u8(0);
  u8(0), put("image/png", 10), u8(3), put("ab", 3), put("FRONT", 5);
  unsigned body = N - 10;
  B[8] = body >> 7 & 0x7f, B[9] = body & 0x7f;
  FILE *f = fopen(PATH, "wb");
  fwrite(B, 1, N, f);
  fclose(f);
  unsigned char *c = media_cover_read(PATH, &len);
  expect_eq(len, 5);
  expect(c && memcmp(c, "FRONT", 5) == 0);
  free(c);
  expect_eq(media_tags_read(PATH, &t), 0);
  remove(PATH);
}

static void test_mp4_covr(void)
{
  struct media_tags t;
  unsigned ftyp, moov, udta, meta, ilst, covr, data, len;
  N = 0;
  box_begin(&ftyp, "ftyp");
  put("M4A \0\0\0\0", 8);
  box_end(ftyp);
  box_begin(&moov, "moov");
  box_begin(&udta, "udta");
  box_begin(&meta, "meta");
  be32(0);
  box_begin(&ilst, "ilst");
  box_begin(&covr, "covr");
  box_begin(&data, "data");
  be32(13), be32(0);
  put("\xFF\xD8JPEGBYTES", 11);
  box_end(data);
  box_end(covr);
  box_end(ilst);
  box_end(meta);
  box_end(udta);
  box_end(moov);
  FILE *f = fopen(PATH, "wb");
  fwrite(B, 1, N, f);
  fclose(f);
  unsigned char *c = media_cover_read(PATH, &len);
  expect_eq(len, 11);
  expect(c && memcmp(c, "\xFF\xD8JPEGBYTES", 11) == 0);
  free(c);
  expect_eq(media_tags_read(PATH, &t), 0);
  remove(PATH);
}

static void flac_stream(unsigned id3_prefix)
{
  unsigned vc, pic;
  N = 0;
  if (id3_prefix) {
    id3_begin(3, 0);
  }
  put("fLaC", 4);
  u8(0), u8(0), u8(0), u8(34); /* STREAMINFO */
  memset(B + N, 0, 34);
  N += 34;
  u8(4);
  vc = N;
  u8(0), u8(0), u8(0);
  le32(4);
  put("owos", 4);
  le32(3);
  vc_entry("Title=Flac Song");
  vc_entry("ARTIST=Flac Artist");
  vc_entry("album=Flac Album");
  B[vc] = (N - vc - 3) >> 16, B[vc + 1] = (N - vc - 3) >> 8, B[vc + 2] = N - vc - 3;
  u8(0x80 | 6);
  pic = N;
  u8(0), u8(0), u8(0);
  flac_picture(3, "image/png", "\x89PNGDATA", 8);
  B[pic] = (N - pic - 3) >> 16, B[pic + 1] = (N - pic - 3) >> 8, B[pic + 2] = N - pic - 3;
}

static void check_flac(void)
{
  struct media_tags t;
  unsigned len;
  FILE *f = fopen(PATH, "wb");
  fwrite(B, 1, N, f);
  fclose(f);
  expect_eq(media_tags_read(PATH, &t), 1);
  expect_string(t.title, "Flac Song");
  expect_string(t.artist, "Flac Artist");
  expect_string(t.album, "Flac Album");
  unsigned char *c = media_cover_read(PATH, &len);
  expect_eq(len, 8);
  expect(c && memcmp(c, "\x89PNGDATA", 8) == 0);
  free(c);
  remove(PATH);
}

static void test_flac(void)
{
  flac_stream(0);
  check_flac();
}

static void test_flac_behind_id3(void)
{
  flac_stream(1);
  check_flac();
}

/* an Ogg page: header type, then lacing values, then `n` body bytes from body */
static void ogg_page(unsigned flags, const unsigned char *lacing, unsigned nseg, const unsigned char *body, unsigned n)
{
  put("OggS", 4);
  u8(0), u8(flags);
  le32(0), le32(0);
  le32(0x1234), le32(0), le32(0);
  u8(nseg);
  put(lacing, nseg);
  put(body, n);
}

static void test_ogg_vorbis_comment_across_pages(void)
{
  struct media_tags t;
  unsigned len;
  static unsigned char pkt[2048];
  unsigned char img[300];
  static char enc[1024];
  memset(img, 7, sizeof img);
  /* picture block */
  N = 0;
  flac_picture(3, "image/jpeg", img, sizeof img);
  b64(B, N, enc);
  N = 0;
  put("\x03vorbis", 7);
  le32(4);
  put("owos", 4);
  le32(4);
  vc_entry("TITLE=Ogg Song");
  vc_entry("artist=Ogg Artist");
  vc_entry("ALBUM=Ogg \xC3\xA9lbum");
  le32(strlen("METADATA_BLOCK_PICTURE=") + strlen(enc));
  put("METADATA_BLOCK_PICTURE=", strlen("METADATA_BLOCK_PICTURE="));
  put(enc, strlen(enc));
  u8(1);
  unsigned plen = N;
  memcpy(pkt, B, plen);

  N = 0;
  unsigned char ident[30] = {1, 'v', 'o', 'r', 'b', 'i', 's'};
  unsigned char lace1[1] = {30};
  ogg_page(0x02, lace1, 1, ident, sizeof ident);
  /* comment packet split: page 2 carries 2*255 bytes unfinished, page 3 the rest */
  unsigned char lace2[2] = {255, 255};
  ogg_page(0, lace2, 2, pkt, 510);
  unsigned rest = plen - 510, nseg = rest / 255 + 1;
  unsigned char lace3[16];
  for (unsigned i = 0; i < nseg; i++)
    lace3[i] = i + 1 < nseg ? 255 : rest % 255;
  ogg_page(0x01, lace3, nseg, pkt + 510, rest);
  unsigned char lace4[1] = {4};
  ogg_page(0, lace4, 1, (const unsigned char *)"setu", 4);

  FILE *f = fopen(PATH, "wb");
  fwrite(B, 1, N, f);
  fclose(f);
  expect_eq(media_tags_read(PATH, &t), 1);
  expect_string(t.title, "Ogg Song");
  expect_string(t.artist, "Ogg Artist");
  expect_string(t.album, "Ogg \xC3\xA9lbum");
  unsigned char *c = media_cover_read(PATH, &len);
  expect_eq(len, 300);
  expect(c && memcmp(c, img, 300) == 0);
  free(c);
  remove(PATH);
}

static void test_opus_tags(void)
{
  struct media_tags t;
  N = 0;
  unsigned char head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
  unsigned char l1[1] = {19};
  ogg_page(0x02, l1, 1, head, sizeof head);
  put("OpusTags", 8);
  le32(2);
  put("ab", 2);
  le32(1);
  vc_entry("TITLE=Opus Song");
  unsigned plen = N - (27 + 1 + 19);
  unsigned char tmp[128];
  memcpy(tmp, B + 27 + 1 + 19, plen); /* move the packet into a page of its own */
  N = 27 + 1 + 19;
  unsigned char l2[1] = {plen};
  ogg_page(0, l2, 1, tmp, plen);
  FILE *f = fopen(PATH, "wb");
  fwrite(B, 1, N, f);
  fclose(f);
  expect_eq(media_tags_read(PATH, &t), 1);
  expect_string(t.title, "Opus Song");
  remove(PATH);
}

/* truncated and bit-flipped FLAC streams must parse without reading out of bounds */
static void test_damaged_flac_is_safe(void)
{
  flac_stream(0);
  unsigned total = N;
  static unsigned char orig[4096];
  memcpy(orig, B, total);
  for (unsigned i = 0; i < total; i++) {
    struct media_tags t;
    unsigned len;
    N = i;
    memcpy(B, orig, total);
    if (i)
      B[i - 1] ^= 0xFF;
    FILE *f = fopen(PATH, "wb");
    fwrite(B, 1, N, f);
    fclose(f);
    media_tags_read(PATH, &t);
    free(media_cover_read(PATH, &len));
    N = total;
    memcpy(B, orig, total);
    B[i] ^= 0xFF;
    f = fopen(PATH, "wb");
    fwrite(B, 1, N, f);
    fclose(f);
    media_tags_read(PATH, &t);
    free(media_cover_read(PATH, &len));
  }
  remove(PATH);
}

static void test_untagged_and_hostile(void)
{
  struct media_tags t;
  N = 64;
  memset(B, 0x55, 64);
  expect_eq(read_file(&t), 0);
  expect_string(t.title, "");

  /* frame size past the tag end and a box larger than the file must not read out of bounds */
  id3_begin(3, 20);
  put("TIT2", 4);
  be32(0x7fffffff);
  u8(0), u8(0), u8(0);
  expect_eq(read_file(&t), 0);

  N = 0;
  be32(0xffffffff);
  put("ftyp", 4);
  expect_eq(read_file(&t), 0);

  expect_eq(media_tags_read("/nonexistent/owos_tags", &t), 0);
}

static void test_long_title_is_cut_on_a_character(void)
{
  struct media_tags t;
  char s[200];
  memset(s, 'a', 94);
  memcpy(s + 94, "\xC3\xA9\xC3\xA9", 4);
  id3_begin(4, 0);
  id3_frame(4, "TIT2", 3, s, 98);
  unsigned body = N - 10;
  B[8] = body >> 7 & 0x7f, B[9] = body & 0x7f;
  expect_eq(read_file(&t), 1);
  expect_eq(strlen(t.title), 94);
}

int main(void)
{
  test("ID3v2.3 UTF-16 and Latin-1", test_id3v23_utf16_and_latin1);
  test("ID3v2.4 UTF-8", test_id3v24_utf8);
  test("ID3v2.2 short ids", test_id3v22);
  test("ID3v1 fallback", test_id3v1_fallback);
  test("MP4 ilst", test_mp4_ilst);
  test("WAV LIST INFO", test_wav_info);
  test("ID3 APIC prefers the front cover", test_id3_apic_prefers_front_cover);
  test("MP4 covr", test_mp4_covr);
  test("FLAC comments and picture", test_flac);
  test("FLAC behind an ID3v2 tag", test_flac_behind_id3);
  test("Ogg Vorbis comment across pages", test_ogg_vorbis_comment_across_pages);
  test("Opus tags", test_opus_tags);
  test("damaged FLAC is safe", test_damaged_flac_is_safe);
  test("untagged and hostile files", test_untagged_and_hostile);
  test("long title cut on a character", test_long_title_is_cut_on_a_character);
  return 0;
}
