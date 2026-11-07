/* ext_jbd.h: jbd2 journal writer for tests/ext_test.c (internal journal inode, v1/v2/v3 checksums, revokes). */
#ifndef EXT_JBD_H
#define EXT_JBD_H

#include "ext_image.h"

#define JB_MAGIC 0xC03B3998u

enum { JB_PLAIN, JB_V1SB, JB_CRC1, JB_V2, JB_V3 };

#define JB_NO_COMMIT 1u
#define JB_BAD_COMMIT_CSUM 2u
#define JB_BAD_DATA_CSUM 4u
#define JB_BAD_DESC_CSUM 8u

struct jb_item {
  uint64_t fsblk;
  uint8_t *data;
};

struct jb {
  struct eb *e;
  uint32_t n, first, seq0, tid, pos, start, mode;
  bool b64;
  uint64_t base;
  uint8_t uuid[16];
  uint32_t seed;
  struct jb_item *items;
  uint32_t nitems;
  uint64_t *revokes;
  uint32_t nrevokes;
  uint32_t crc;
};

static uint8_t *jb_block(struct jb *j, uint32_t pos) { return eb_blk(j->e, j->base + pos); }

static uint32_t jb_adv(const struct jb *j, uint32_t pos)
{
  pos++;
  return pos >= j->n ? j->first : pos;
}

static bool jb_csum23(const struct jb *j) { return j->mode == JB_V2 || j->mode == JB_V3; }

static uint32_t jb_tag_bytes(const struct jb *j)
{
  if (j->mode == JB_V3)
    return 16;
  uint32_t t = j->b64 ? 12 : 8;
  return j->mode == JB_V2 ? t + 2 : t;
}

/* Journal inode 8 with n blocks; start_at is where the first transaction is written. */
static void jb_init(struct jb *j, struct eb *e, uint32_t n, uint32_t mode, bool b64, uint32_t seq0, uint32_t start_at)
{
  memset(j, 0, sizeof *j);
  j->e = e;
  j->n = n;
  j->first = 1;
  j->mode = mode;
  j->b64 = b64;
  j->seq0 = j->tid = seq0;
  j->pos = start_at ? start_at : 1;
  for (int i = 0; i < 16; i++)
    j->uuid[i] = (uint8_t)(0x31 + i * 7);
  j->seed = eb_crc32c(~0u, j->uuid, 16);
  uint32_t ino = eb_mkino(e, 8, EB_REG | 0600);
  j->base = eb_alloc_n(e, n);
  eb_map_run(e, ino, 0, j->base, n);
  eb_add_blocks(e, ino, n);
  eb_set_size(e, ino, (uint64_t)n * e->bs);
  e->journal_inum = 8;
  e->compat |= EB_COMPAT_JOURNAL;
  e->incompat |= EB_INC_RECOVER;
}

static void jb_begin(struct jb *j)
{
  for (uint32_t i = 0; i < j->nitems; i++)
    free(j->items[i].data);
  free(j->items);
  free(j->revokes);
  j->items = NULL;
  j->revokes = NULL;
  j->nitems = j->nrevokes = 0;
}

static void jb_add(struct jb *j, uint64_t fsblk, const void *data)
{
  j->items = realloc(j->items, (j->nitems + 1) * sizeof *j->items);
  j->items[j->nitems].fsblk = fsblk;
  j->items[j->nitems].data = malloc(j->e->bs);
  memcpy(j->items[j->nitems].data, data, j->e->bs);
  j->nitems++;
}

static void jb_revoke(struct jb *j, uint64_t fsblk)
{
  j->revokes = realloc(j->revokes, (j->nrevokes + 1) * sizeof *j->revokes);
  j->revokes[j->nrevokes++] = fsblk;
}

static void jb_hdr(uint8_t *b, uint32_t type, uint32_t tid)
{
  wb32(b, JB_MAGIC);
  wb32(b + 4, type);
  wb32(b + 8, tid);
}

static void jb_tail_csum(struct jb *j, uint8_t *b, bool bad)
{
  uint32_t bs = j->e->bs;
  wb32(b + bs - 4, 0);
  uint32_t c = eb_crc32c(j->seed, b, bs);
  wb32(b + bs - 4, bad ? ~c : c);
}

static void jb_commit(struct jb *j, uint32_t flags)
{
  struct eb *e = j->e;
  uint32_t bs = e->bs;
  if (!j->start)
    j->start = j->pos;
  j->crc = ~0u;
  uint32_t csz = jb_csum23(j) ? 4 : 0, rec = j->b64 ? 8 : 4;
  for (uint32_t i = 0; i < j->nrevokes;) {
    uint8_t *b = jb_block(j, j->pos);
    memset(b, 0, bs);
    jb_hdr(b, 5, j->tid);
    uint32_t off = 16;
    while (i < j->nrevokes && off + rec <= bs - csz) {
      if (j->b64) {
        wb32(b + off, (uint32_t)(j->revokes[i] >> 32));
        wb32(b + off + 4, (uint32_t)j->revokes[i]);
      } else {
        wb32(b + off, (uint32_t)j->revokes[i]);
      }
      off += rec;
      i++;
    }
    wb32(b + 12, off);
    if (csz)
      jb_tail_csum(j, b, false);
    j->pos = jb_adv(j, j->pos);
  }
  uint32_t tb = jb_tag_bytes(j), limit = bs - csz;
  for (uint32_t i = 0; i < j->nitems;) {
    uint8_t *d = jb_block(j, j->pos);
    uint32_t dpos = j->pos;
    memset(d, 0, bs);
    jb_hdr(d, 1, j->tid);
    uint32_t off = 12, first = i, cnt = 0;
    while (i < j->nitems && off + tb + (i == first ? 16 : 0) <= limit) {
      off += tb + (i == first ? 16 : 0);
      i++;
      cnt++;
    }
    j->pos = jb_adv(j, j->pos);
    off = 12;
    for (uint32_t k = 0; k < cnt; k++) {
      struct jb_item *it = &j->items[first + k];
      uint8_t *t = d + off;
      uint32_t fl = (k ? 2u : 0u) | (k == cnt - 1 ? 8u : 0u);
      uint8_t *dest = jb_block(j, j->pos);
      memcpy(dest, it->data, bs);
      if (rb32(dest) == JB_MAGIC) {
        memset(dest, 0, 4);
        fl |= 1;
      }
      uint32_t c = 0;
      if (jb_csum23(j)) {
        uint8_t s[4];
        wb32(s, j->tid);
        c = eb_crc32c(j->seed, s, 4);
        c = eb_crc32c(c, dest, bs);
      }
      wb32(t, (uint32_t)it->fsblk);
      if (j->mode == JB_V3) {
        wb32(t + 4, fl);
        wb32(t + 8, (uint32_t)(it->fsblk >> 32));
        wb32(t + 12, c);
      } else {
        wb16(t + 4, c);
        wb16(t + 6, fl);
        if (j->b64)
          wb32(t + 8, (uint32_t)(it->fsblk >> 32));
      }
      if (flags & JB_BAD_DATA_CSUM && k == 0)
        dest[bs - 1] ^= 0x55;
      off += tb + (k == 0 ? 16 : 0);
      if (k == 0)
        memcpy(d + 12 + tb, j->uuid, 16);
      j->pos = jb_adv(j, j->pos);
    }
    if (csz)
      jb_tail_csum(j, d, (flags & JB_BAD_DESC_CSUM) != 0);
    if (j->mode == JB_CRC1) {
      j->crc = eb_crc32be(j->crc, d, bs);
      uint32_t p = dpos;
      for (uint32_t k = 0; k < cnt; k++) {
        p = jb_adv(j, p);
        j->crc = eb_crc32be(j->crc, jb_block(j, p), bs);
      }
    }
  }
  if (!(flags & JB_NO_COMMIT)) {
    uint8_t *c = jb_block(j, j->pos);
    memset(c, 0, bs);
    jb_hdr(c, 2, j->tid);
    wb32(c + 52, 1700000100u);
    if (j->mode == JB_CRC1) {
      c[12] = 1;
      c[13] = 4;
      wb32(c + 16, flags & JB_BAD_COMMIT_CSUM ? ~j->crc : j->crc);
    } else if (jb_csum23(j)) {
      uint32_t x = eb_crc32c(j->seed, c, bs);
      wb32(c + 16, flags & JB_BAD_COMMIT_CSUM ? ~x : x);
    }
    j->pos = jb_adv(j, j->pos);
  }
  j->tid++;
}

static void jb_finish(struct jb *j)
{
  struct eb *e = j->e;
  uint8_t *s = jb_block(j, 0);
  uint32_t bs = e->bs;
  memset(s, 0, bs);
  jb_hdr(s, j->mode == JB_V1SB ? 3 : 4, 0);
  wb32(s + 12, bs);
  wb32(s + 16, j->n);
  wb32(s + 20, j->first);
  wb32(s + 24, j->seq0);
  wb32(s + 28, j->start);
  if (j->mode != JB_V1SB) {
    uint32_t compat = j->mode == JB_CRC1 ? 1 : 0;
    uint32_t inc = 1 | (j->b64 ? 2 : 0) | (j->mode == JB_V2 ? 8 : 0) | (j->mode == JB_V3 ? 0x10 : 0);
    wb32(s + 36, compat);
    wb32(s + 40, inc);
    memcpy(s + 48, j->uuid, 16);
    wb32(s + 64, 1);
    if (jb_csum23(j)) {
      s[80] = 4;
      wb32(s + 0xFC, 0);
      wb32(s + 0xFC, eb_crc32c(~0u, s, 1024));
    }
  }
}

#endif
