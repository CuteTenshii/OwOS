/* rar_bits.c - bit-level input from the packed stream and canonical Huffman tables. */
#include "owos.h"
#include "rar_int.h"

#define BR_CAP 0x10000u

int br_init(struct bitr *b, struct rar *r)
{
  memset(b, 0, sizeof *b);
  b->r = r;
  b->cap = BR_CAP;
  b->buf = rar_alloc(r, BR_CAP + 16);
  return b->buf ? RAR_OK : RAR_ENOMEM;
}

void br_free(struct bitr *b)
{
  if (b->buf)
    rar_unalloc(b->r, b->buf, b->cap + 16);
  b->buf = NULL;
}

void br_reset(struct bitr *b)
{
  b->len = b->pos = 0;
  b->base = 0;
  b->eof = false;
  b->err = 0;
  memset(b->buf, 0, 16);
}

void br_refill(struct bitr *b)
{
  uint32_t used = b->pos >> 3;
  if (used > b->len)
    used = b->len;
  if (used) {
    memmove(b->buf, b->buf + used, b->len - used);
    b->base += used;
    b->len -= used;
    b->pos -= used * 8;
  }
  while (!b->eof && b->len < b->cap && b->len - (b->pos >> 3) < 64) {
    int n = pk_read(b->r, b->buf + b->len, b->cap - b->len);
    if (n <= 0) {
      b->eof = true;
      if (n < 0)
        b->err = n;
    } else
      b->len += (uint32_t)n;
  }
  memset(b->buf + b->len, 0, 16);
}

bool huff_build(struct huff *h, const uint8_t *lens, unsigned n)
{
  uint16_t next[17];
  memset(h->count, 0, sizeof h->count);
  memset(h->fast, 0, sizeof h->fast);
  for (unsigned i = 0; i < n; i++) {
    if (lens[i] > 15)
      return false;
    h->count[lens[i]]++;
  }
  h->count[0] = 0;
  int left = 1;
  for (int len = 1; len <= 15; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0)
      return false;
  }
  uint16_t offs[17];
  offs[1] = 0;
  for (int len = 1; len < 16; len++)
    offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
  memcpy(next, offs, sizeof next);
  for (unsigned i = 0; i < n; i++)
    if (lens[i])
      h->sym[next[lens[i]]++] = (uint16_t)i;
  uint32_t code = 0;
  unsigned idx = 0;
  for (int len = 1; len <= 15; len++) {
    for (unsigned k = 0; k < h->count[len]; k++, code++, idx++) {
      if (len <= HUFF_FAST) {
        uint32_t lo = code << (HUFF_FAST - len), hi = lo + (1u << (HUFF_FAST - len));
        for (uint32_t j = lo; j < hi; j++)
          h->fast[j] = (uint16_t)((len << 9) | h->sym[idx]);
      }
    }
    code <<= 1;
  }
  return true;
}
