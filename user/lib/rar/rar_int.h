/* rar_int.h - internal definitions shared by the RAR library files (not an API). */
#ifndef OWOS_RAR_INT_H
#define OWOS_RAR_INT_H

#include "owos.h"
#include "rar.h"
#include <stddef.h>

#define RAR_DEFAULT_MEM (512u << 20)
#define RAR_MAX_HDR 0x200000u

struct rar_aes {
  uint32_t rk[60];
  int rounds;
};

void rar_aes_init(struct rar_aes *a, const uint8_t *key, unsigned keylen);
void rar_aes_cbc_decrypt(const struct rar_aes *a, uint8_t iv[16], uint8_t *buf, uint32_t len);
bool rar5_kdf(const uint8_t *pw, uint32_t pwlen, const uint8_t salt[16], unsigned lg2, uint8_t key[32],
              uint8_t hashkey[32], uint8_t check[8]);
void rar5_hash_mac(const uint8_t hashkey[32], const uint8_t *msg, uint32_t len, uint8_t out[32]);
void rar3_kdf(const uint8_t *pw16, uint32_t pwlen, const uint8_t salt[8], uint8_t key[16], uint8_t iv[16]);

/* BLAKE2sp (RAR5 file hash). */
struct b2s {
  uint32_t h[8];
  uint32_t t[2];
  uint32_t f[2];
  uint8_t buf[64];
  uint32_t buflen;
};
struct b2sp {
  struct b2s leaf[8];
  struct b2s root;
  uint8_t buf[512];
  uint32_t buflen;
};
void b2sp_init(struct b2sp *s);
void b2sp_update(struct b2sp *s, const void *data, size_t len);
void b2sp_final(struct b2sp *s, uint8_t out[32]);

/* ------------------------------------------------------------------ bit input */

struct bitr {
  struct rar *r;
  uint8_t *buf;
  uint32_t cap, len, pos; /* pos in bits within buf */
  uint64_t base;          /* stream offset of buf[0] */
  bool eof;
  int err;
};

int br_init(struct bitr *b, struct rar *r);
void br_free(struct bitr *b);
void br_reset(struct bitr *b);
void br_refill(struct bitr *b);

static inline void br_fill(struct bitr *b)
{
  if (!b->eof && (b->pos >> 3) + 16 > b->len)
    br_refill(b);
}

static inline uint32_t br_peek(struct bitr *b, unsigned n)
{
  br_fill(b);
  uint32_t at = b->pos >> 3;
  if (at > b->len)
    return 0;
  const uint8_t *p = b->buf + at;
  uint64_t v = ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
               ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | p[7];
  v <<= (b->pos & 7);
  return (uint32_t)(v >> (64 - n));
}

static inline void br_skip(struct bitr *b, unsigned n) { b->pos += n; }

static inline uint32_t br_bits(struct bitr *b, unsigned n)
{
  uint32_t v = br_peek(b, n);
  b->pos += n;
  return v;
}

static inline void br_align(struct bitr *b) { b->pos = (b->pos + 7) & ~7u; }

/* True once the reader consumed more than the stream held. */
static inline bool br_over(const struct bitr *b) { return b->eof && b->pos > b->len * 8; }

static inline uint64_t br_abs_bits(const struct bitr *b) { return b->base * 8 + b->pos; }

/* ------------------------------------------------------------------ Huffman */

#define HUFF_FAST 10
#define HUFF_MAXSYM 320

struct huff {
  uint16_t fast[1 << HUFF_FAST]; /* len << 9 | symbol, 0 = not in the fast table */
  uint16_t count[16];
  uint16_t sym[HUFF_MAXSYM];
};

bool huff_build(struct huff *h, const uint8_t *lens, unsigned n);

static inline int huff_decode(struct bitr *b, const struct huff *h)
{
  uint32_t v = br_peek(b, 15);
  uint16_t e = h->fast[v >> (15 - HUFF_FAST)];
  if (e) {
    br_skip(b, e >> 9);
    return e & 511;
  }
  uint32_t first = 0, index = 0;
  for (unsigned len = 1; len <= 15; len++) {
    uint32_t code = v >> (15 - len);
    uint32_t cnt = h->count[len];
    if (code - first < cnt) {
      br_skip(b, len);
      return h->sym[index + code - first];
    }
    index += cnt;
    first = (first + cnt) << 1;
  }
  return -1;
}

/* ------------------------------------------------------------------ filters and window */

enum {
  FT_DELTA,
  FT_E8,
  FT_E8E9,
  FT_ARM,
  FT_ITANIUM,
  FT_RGB,
  FT_AUDIO,
};

struct lzfilter {
  uint64_t start; /* stream offset of the block */
  uint32_t len;
  uint32_t r[7]; /* VM registers R0..R6 (RAR3); r[0] holds the channel count for RAR5 delta */
  uint8_t type;
  bool v5;
};

struct lzwin {
  uint8_t *buf;
  uint32_t size, cap; /* both powers of two */
  uint64_t abs, out_abs, file_base, next_flush;
  struct lzfilter *f;
  unsigned nf, fcap, fhead;
  uint8_t *tmp;
  size_t tmpcap;
};

/* Run a filter over buf[0..len) (scratch >= len bytes); returns the result buffer, NULL on bad parameters. */
const uint8_t *rar_filter_run(const struct lzfilter *f, uint8_t *buf, uint8_t *scratch, uint32_t len,
                              uint32_t file_off);
uint32_t rar_filter_max_len(uint8_t type);

/* ------------------------------------------------------------------ PPMd (variant H) */

struct ppmd;
struct ppmd_rc {
  uint32_t (*threshold)(struct ppmd_rc *rc, uint32_t total); /* UINT32_MAX on a coder fault */
  void (*decode)(struct ppmd_rc *rc, uint32_t start, uint32_t size);
  uint32_t (*bit)(struct ppmd_rc *rc, uint32_t size0, uint32_t total); /* 0/1, 2 on a coder fault */
};

struct ppmd *ppmd_new(uint32_t mem_bytes);
void ppmd_free(struct ppmd *p);
void ppmd_init(struct ppmd *p, unsigned max_order);
int ppmd_decode(struct ppmd *p, struct ppmd_rc *rc); /* 0..255, -1 end mark, -2 data error */

struct ppmd_rar_rc {
  struct ppmd_rc rc;
  uint32_t low, code, range;
  int (*getbyte)(void *ctx);
  void *ctx;
};
void ppmd_rar_rc_init(struct ppmd_rar_rc *c, int (*getbyte)(void *), void *ctx);

/* ------------------------------------------------------------------ archive state */

struct rar_vol {
  struct rar_src src;
  bool open;
  int idx;
};

struct rar_enc {
  uint8_t ver; /* 0 none, 3 or 5 */
  uint8_t salt[16];
  uint8_t iv[16];
  uint8_t lg2;
  bool has_check, hash_mac;
  uint8_t check[8];
};

struct rar_chunk {
  int vol;
  uint64_t off, size;
  struct rar_enc enc;
};

struct rar_keycache {
  bool valid;
  uint8_t ver;
  uint8_t salt[16];
  uint8_t lg2;
  uint8_t key[32], hashkey[32], check[8], iv[16];
};

struct lz5 {
  struct huff main, dist, low, rep;
  uint32_t dcache[4];
  uint32_t last_len;
  bool have_tables;
  bool filters_v7;
};

struct vmprog {
  uint8_t type;
  uint32_t old_len;
};

struct lz4 {
  struct huff main, dist, low, rep;
  uint8_t old_table[404];
  bool tables_read, ppm_active;
  uint32_t old_dist[4];
  uint32_t last_len;
  uint32_t prev_low;
  int low_rep;
  struct ppmd *ppm;
  struct ppmd_rar_rc rc;
  uint32_t ppm_mem;
  int esc_char;
  struct vmprog *progs;
  unsigned nprogs, pcap, last_prog;
};

struct rar_file {
  uint64_t size, packed;
  bool size_unknown;
  uint32_t crc, mtime, attr, mode;
  bool has_crc, has_b2;
  uint8_t b2[32];
  uint8_t method, host, version;
  bool is_dir, is_symlink, is_hardlink, encrypted, solid, split;
  uint64_t dict;
  bool comp_solid, v7;
  char *name, *link;
  struct rar_chunk *chunks;
  unsigned nchunks, cchunks;
  bool incomplete;
  uint8_t hashkey[32];
  bool hash_mac;
};

struct rar_pk {
  unsigned ci;
  uint64_t left, off;
  bool started, dec;
  struct rar_aes aes;
  uint8_t iv[16];
  uint8_t rem[16];
  unsigned remn;
};

struct rar_out {
  int (*write)(void *ctx, const void *buf, uint32_t len);
  void *ctx;
  uint32_t crc;
  struct b2sp b2;
  bool use_b2, discard;
  uint64_t produced; /* bytes of the current entry */
};

struct rar {
  struct rar_cfg cfg;
  char *first_name;
  char *password;
  uint64_t max_mem, mem;
  struct rar_vol hv, dv;
  int fmt;
  bool solid, multivol, hdr_enc, vol_missing, list_only;
  uint64_t hoff; /* next header offset in hv */
  uint8_t *hbuf;
  size_t hcap;
  bool main_seen;
  struct rar_keycache kc, hkc;
  struct rar_aes haes; /* header decryption key */
  bool haes_ready;
  uint8_t hiv[16];
  char *missing_name;
  uint64_t total_out;

  struct rar_file cur;
  bool have_cur, cur_done;
  bool broken; /* solid state lost (list-only skip, decode error) */

  struct rar_pk pk;
  struct rar_out out;
  struct bitr br;
  bool br_ready;
  struct lzwin win;
  struct lz5 l5;
  struct lz4 l4;
  uint8_t *iobuf;
};

void *rar_alloc(struct rar *r, size_t n);
void *rar_realloc(struct rar *r, void *p, size_t old, size_t n);
void rar_unalloc(struct rar *r, void *p, size_t n);

int rar_vol_read(struct rar *r, struct rar_vol *v, uint64_t off, void *buf, uint32_t len);
int rar_open_volume(struct rar *r, struct rar_vol *v, int idx);
void rar_vol_close(struct rar_vol *v);

int rar_get_key(struct rar *r, const struct rar_enc *e, struct rar_keycache *kc);

/* packed stream and output */
int pk_start(struct rar *r);
int pk_read(struct rar *r, uint8_t *buf, uint32_t n);
int out_emit(struct rar *r, const void *buf, uint32_t n);

/* window */
int lzw_init(struct rar *r, uint32_t cap, uint64_t hint);
void lzw_free(struct rar *r);
int lzw_flush(struct rar *r, bool final);
int lzw_room(struct rar *r, uint32_t n);
int lzw_add_filter(struct rar *r, const struct lzfilter *f);

static inline int lzw_reserve(struct rar *r, uint32_t n)
{
  struct lzwin *w = &r->win;
  if (w->abs - w->out_abs + n <= w->size && (w->size == w->cap || w->abs + n <= w->size))
    return 0;
  return lzw_room(r, n);
}

static inline void lzw_put(struct lzwin *w, uint8_t c) { w->buf[(uint32_t)w->abs++ & (w->size - 1)] = c; }

static inline void lzw_copy(struct lzwin *w, uint32_t dist, uint32_t len)
{
  uint32_t mask = w->size - 1;
  uint32_t dst = (uint32_t)w->abs & mask;
  uint32_t src = (dst - dist) & mask;
  if (dst + len <= w->size && src + len <= w->size && (dist >= len)) {
    memcpy(w->buf + dst, w->buf + src, len);
  } else {
    for (uint32_t i = 0; i < len; i++) {
      w->buf[dst] = w->buf[src];
      dst = (dst + 1) & mask;
      src = (src + 1) & mask;
    }
  }
  w->abs += len;
}

/* decoders: decode the current entry's packed stream into the output */
int rar5_decode(struct rar *r);
int rar4_decode(struct rar *r);
void rar5_reset(struct rar *r);
void rar4_free(struct rar *r);

/* a parsed file header: the entry fields plus its first data chunk */
struct fh {
  struct rar_file f;
  struct rar_chunk c;
  bool split_before, split_after;
};

void rar_file_free(struct rar_file *f);
int rar_scan(struct rar *r, bool stop_at_main, struct fh *fh);
int rar_find_sig(struct rar *r, struct rar_vol *v, uint64_t *end, int *fmt);
int rar_start_volume(struct rar *r);
int rar_next_volume(struct rar *r);

#endif /* OWOS_RAR_INT_H */
