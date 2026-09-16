/* protobuf.c — Protocol Buffers wire-format codec. See protobuf.h. */
#include "protobuf.h"
#include "owos.h" /* libc: malloc/realloc/free, memcpy */

#define PB_FIELD_MAX 0x1FFFFFFFu /* field numbers are 29 bits */

/* ---- Reader ------------------------------------------------------------- */

void pb_reader_init(pb_reader *r, const void *buf, uint32_t len)
{
  r->buf = (const uint8_t *)buf;
  r->len = len;
  r->pos = 0;
  r->error = 0;
}

int pb_reader_eof(const pb_reader *r) { return r->error || r->pos >= r->len; }

/* Decode a base-128 varint. Bounded to 10 bytes (a 64-bit varint's max) so a
 * run of high-bit-set bytes can't spin past the end or overflow. */
static int read_varint(pb_reader *r, uint64_t *out)
{
  uint64_t v = 0;
  int shift = 0;
  while (shift < 64) {
    if (r->pos >= r->len) {
      r->error = 1;
      return -1;
    }
    uint8_t b = r->buf[r->pos++];
    v |= (uint64_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) {
      *out = v;
      return 1;
    }
    shift += 7;
  }
  r->error = 1; /* 10th byte still had continuation bit */
  return -1;
}

int pb_read_varint(pb_reader *r, uint64_t *v)
{
  if (r->error)
    return -1;
  return read_varint(r, v);
}

int pb_read_tag(pb_reader *r, uint32_t *field, int *wire_type)
{
  if (r->error)
    return -1;
  if (r->pos >= r->len)
    return 0; /* clean end */
  uint64_t tag;
  if (read_varint(r, &tag) < 0)
    return -1;
  uint32_t fno = (uint32_t)(tag >> 3);
  int wt = (int)(tag & 7);
  if (fno == 0 || fno > PB_FIELD_MAX) {
    r->error = 1;
    return -1;
  }
  *field = fno;
  *wire_type = wt;
  return 1;
}

int pb_read_i32(pb_reader *r, uint32_t *v)
{
  if (r->error)
    return -1;
  if (r->pos + 4 > r->len) {
    r->error = 1;
    return -1;
  }
  const uint8_t *p = r->buf + r->pos; /* little-endian on the wire */
  *v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  r->pos += 4;
  return 1;
}

int pb_read_i64(pb_reader *r, uint64_t *v)
{
  if (r->error)
    return -1;
  if (r->pos + 8 > r->len) {
    r->error = 1;
    return -1;
  }
  const uint8_t *p = r->buf + r->pos;
  uint32_t lo = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  uint32_t hi = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
  *v = (uint64_t)lo | (uint64_t)hi << 32;
  r->pos += 8;
  return 1;
}

int pb_read_bytes(pb_reader *r, const uint8_t **p, uint32_t *n)
{
  if (r->error)
    return -1;
  uint64_t len;
  if (read_varint(r, &len) < 0)
    return -1;
  /* len is bounded by the buffer, so it can't legitimately exceed 32 bits. */
  if (len > (uint64_t)(r->len - r->pos)) {
    r->error = 1;
    return -1;
  }
  *p = r->buf + r->pos;
  *n = (uint32_t)len;
  r->pos += (uint32_t)len;
  return 1;
}

int pb_read_message(pb_reader *r, pb_reader *sub)
{
  const uint8_t *p;
  uint32_t n;
  if (pb_read_bytes(r, &p, &n) < 0)
    return -1;
  pb_reader_init(sub, p, n);
  return 1;
}

static int pb_skip_d(pb_reader *r, int wire_type, int depth)
{
  if (r->error)
    return -1;
  if (depth > 64) {
    r->error = 1;
    return -1;
  } /* group nesting too deep -> stack guard */
  switch (wire_type) {
  case PB_VARINT: {
    uint64_t v;
    return read_varint(r, &v);
  }
  case PB_I64: {
    uint64_t v;
    return pb_read_i64(r, &v);
  }
  case PB_I32: {
    uint32_t v;
    return pb_read_i32(r, &v);
  }
  case PB_LEN: {
    const uint8_t *p;
    uint32_t n;
    return pb_read_bytes(r, &p, &n);
  }
  case PB_SGROUP: /* skip to the matching EGROUP */
    for (;;) {
      uint32_t f;
      int wt;
      int t = pb_read_tag(r, &f, &wt);
      if (t <= 0) {
        r->error = 1;
        return -1;
      }
      if (wt == PB_EGROUP)
        return 1;
      if (pb_skip_d(r, wt, depth + 1) < 0)
        return -1; /* nested groups recurse */
    }
  default: /* stray EGROUP or bad type */
    r->error = 1;
    return -1;
  }
}

int pb_skip(pb_reader *r, int wire_type) { return pb_skip_d(r, wire_type, 0); }

int32_t pb_zigzag32(uint64_t v)
{
  uint32_t u = (uint32_t)v;
  return (int32_t)((u >> 1) ^ (~(u & 1) + 1)); /* -(u & 1) without UB */
}

int64_t pb_zigzag64(uint64_t v) { return (int64_t)((v >> 1) ^ (~(v & 1) + 1)); }

float pb_as_float(uint32_t v)
{
  float f;
  memcpy(&f, &v, 4);
  return f;
}

double pb_as_double(uint64_t v)
{
  double d;
  memcpy(&d, &v, 8);
  return d;
}

/* ---- Writer ------------------------------------------------------------- */

void pb_writer_init(pb_writer *w)
{
  w->buf = NULL;
  w->len = 0;
  w->cap = 0;
  w->error = 0;
}

void pb_writer_free(pb_writer *w)
{
  free(w->buf);
  pb_writer_init(w);
}

uint8_t *pb_writer_take(pb_writer *w, uint32_t *len)
{
  if (w->error)
    return NULL;
  uint8_t *b = w->buf;
  if (len)
    *len = w->len;
  pb_writer_init(w);
  return b;
}

/* Ensure room for `extra` more bytes, growing geometrically. */
static int reserve(pb_writer *w, uint32_t extra)
{
  if (w->error)
    return -1;
  if (w->len + extra < w->len) {
    w->error = 1;
    return -1;
  } /* overflow */
  if (w->len + extra <= w->cap)
    return 0;
  uint32_t cap = w->cap ? w->cap : 32;
  while (cap < w->len + extra) {
    uint32_t nc = cap * 2;
    if (nc < cap) {
      w->error = 1;
      return -1;
    }
    cap = nc;
  }
  uint8_t *nb = realloc(w->buf, cap);
  if (!nb) {
    w->error = 1;
    return -1;
  }
  w->buf = nb;
  w->cap = cap;
  return 0;
}

static int put_byte(pb_writer *w, uint8_t b)
{
  if (reserve(w, 1) < 0)
    return -1;
  w->buf[w->len++] = b;
  return 0;
}

int pb_put_varint(pb_writer *w, uint64_t v)
{
  if (w->error)
    return -1;
  do {
    uint8_t b = v & 0x7f;
    v >>= 7;
    if (v)
      b |= 0x80;
    if (put_byte(w, b) < 0)
      return -1;
  } while (v);
  return 0;
}

int pb_put_i32(pb_writer *w, uint32_t v)
{
  if (reserve(w, 4) < 0)
    return -1;
  w->buf[w->len++] = v & 0xff;
  w->buf[w->len++] = (v >> 8) & 0xff;
  w->buf[w->len++] = (v >> 16) & 0xff;
  w->buf[w->len++] = (v >> 24) & 0xff;
  return 0;
}

int pb_put_i64(pb_writer *w, uint64_t v)
{
  if (pb_put_i32(w, (uint32_t)v) < 0)
    return -1;
  return pb_put_i32(w, (uint32_t)(v >> 32));
}

static int put_tag(pb_writer *w, uint32_t field, int wire_type)
{
  if (field == 0 || field > PB_FIELD_MAX) {
    w->error = 1;
    return -1;
  }
  return pb_put_varint(w, ((uint64_t)field << 3) | (uint32_t)wire_type);
}

int pb_write_varint(pb_writer *w, uint32_t field, uint64_t v)
{
  if (put_tag(w, field, PB_VARINT) < 0)
    return -1;
  return pb_put_varint(w, v);
}

int pb_write_signed(pb_writer *w, uint32_t field, int64_t v)
{
  return pb_write_varint(w, field, (uint64_t)v); /* 2's-complement, sign-extended */
}

int pb_write_sint(pb_writer *w, uint32_t field, int64_t v)
{
  uint64_t zz = ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);
  return pb_write_varint(w, field, zz);
}

int pb_write_bool(pb_writer *w, uint32_t field, int b) { return pb_write_varint(w, field, b ? 1 : 0); }

int pb_write_i32(pb_writer *w, uint32_t field, uint32_t v)
{
  if (put_tag(w, field, PB_I32) < 0)
    return -1;
  return pb_put_i32(w, v);
}

int pb_write_i64(pb_writer *w, uint32_t field, uint64_t v)
{
  if (put_tag(w, field, PB_I64) < 0)
    return -1;
  return pb_put_i64(w, v);
}

int pb_write_float(pb_writer *w, uint32_t field, float f)
{
  uint32_t v;
  memcpy(&v, &f, 4);
  return pb_write_i32(w, field, v);
}

int pb_write_double(pb_writer *w, uint32_t field, double d)
{
  uint64_t v;
  memcpy(&v, &d, 8);
  return pb_write_i64(w, field, v);
}

int pb_write_bytes(pb_writer *w, uint32_t field, const void *data, uint32_t n)
{
  if (put_tag(w, field, PB_LEN) < 0)
    return -1;
  if (pb_put_varint(w, n) < 0)
    return -1;
  if (reserve(w, n) < 0)
    return -1;
  if (n)
    memcpy(w->buf + w->len, data, n);
  w->len += n;
  return 0;
}

int pb_write_string(pb_writer *w, uint32_t field, const char *s)
{
  uint32_t n = 0;
  while (s[n])
    n++;
  return pb_write_bytes(w, field, s, n);
}

int pb_write_message(pb_writer *w, uint32_t field, const pb_writer *msg)
{
  if (msg->error) {
    w->error = 1;
    return -1;
  }
  return pb_write_bytes(w, field, msg->buf, msg->len);
}
