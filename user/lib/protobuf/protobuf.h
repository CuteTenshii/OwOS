/* protobuf.h — Protocol Buffers wire-format codec, schema-agnostic.
 *
 * Protobuf's wire format carries no field names or types: every field is a
 * (field-number, wire-type) tag followed by a value whose meaning only a
 * schema knows. So the general primitive — the part that isn't specific to any
 * one .proto — is exactly the encode/decode of that wire format. This library
 * provides it: a streaming reader that walks a buffer field by field, and a
 * growable writer that appends fields. The consumer (a Cast CastMessage, a
 * Google API reply, …) owns its schema and reads/writes fields by number.
 *
 * Only libc (malloc/realloc/free, mem*) is needed. Link with $(PROTOBUF).
 *
 * Reader ownership: zero-copy. pb_read_bytes hands back a span pointing into
 * the source buffer, which must outlive the reader. The reader never reads out
 * of bounds and rejects truncated/overlong varints — safe on untrusted input.
 *
 * Writer ownership: pb_writer owns a malloc'd buffer that grows as you append;
 * pb_writer_free releases it. pb_writer_take detaches the buffer to the caller.
 *
 * Wire coverage: all four live wire types (varint, 64-bit, length-delimited,
 * 32-bit). The deprecated group types (3/4) are recognised for skipping but not
 * produced. Field numbers are the valid 1..2^29-1 range; larger tags error.
 */
#ifndef OWOS_PROTOBUF_H
#define OWOS_PROTOBUF_H

#include <stdint.h>

/* Wire types (the low 3 bits of a field tag). */
enum {
  PB_VARINT = 0, /* int32/64, uint32/64, sint32/64 (zigzag), bool, enum */
  PB_I64 = 1,    /* fixed64, sfixed64, double                           */
  PB_LEN = 2,    /* string, bytes, embedded message, packed repeated    */
  PB_SGROUP = 3, /* group start  (deprecated; skip-only)                */
  PB_EGROUP = 4, /* group end    (deprecated; skip-only)                */
  PB_I32 = 5,    /* fixed32, sfixed32, float                            */
};

/* ---- Reader --------------------------------------------------------------
 * Walk a buffer field by field. `error` latches on the first malformed read;
 * once set every subsequent call is a no-op returning failure. */
typedef struct {
  const uint8_t *buf;
  uint32_t len;
  uint32_t pos;
  int error; /* non-zero once a decode error is seen */
} pb_reader;

void pb_reader_init(pb_reader *r, const void *buf, uint32_t len);

/* True once the whole buffer has been consumed (or on error). */
int pb_reader_eof(const pb_reader *r);

/* Read the next field tag. Returns 1 and fills *field and *wire_type on a
 * field, 0 at clean end of buffer, -1 on a malformed tag (also sets r->error).
 */
int pb_read_tag(pb_reader *r, uint32_t *field, int *wire_type);

/* Value readers — call the one matching the tag's wire type. Each returns 1 on
 * success, -1 on truncation/overflow (setting r->error). */
int pb_read_varint(pb_reader *r, uint64_t *v);                   /* PB_VARINT */
int pb_read_i32(pb_reader *r, uint32_t *v);                      /* PB_I32    */
int pb_read_i64(pb_reader *r, uint64_t *v);                      /* PB_I64    */
int pb_read_bytes(pb_reader *r, const uint8_t **p, uint32_t *n); /* PB_LEN, zero-copy */

/* Open an embedded message: reads the length prefix and initialises `sub` over
 * the nested bytes (sharing the parent's buffer). Returns 1 / -1. PB_LEN. */
int pb_read_message(pb_reader *r, pb_reader *sub);

/* Skip the value of a field with the given wire type (for fields you ignore).
 * Handles groups by matching PB_SGROUP/PB_EGROUP. Returns 1 / -1. */
int pb_skip(pb_reader *r, int wire_type);

/* Reinterpretation helpers for varint/fixed payloads (no I/O, never fail). */
int32_t pb_zigzag32(uint64_t v); /* sint32 decode */
int64_t pb_zigzag64(uint64_t v); /* sint64 decode */
float pb_as_float(uint32_t v);   /* fixed32 bits -> float  */
double pb_as_double(uint64_t v); /* fixed64 bits -> double */

/* ---- Writer --------------------------------------------------------------
 * Appends fields to a growable buffer. `error` latches on allocation failure;
 * once set, every append is a no-op and the buffer is left valid-but-partial.
 */
typedef struct {
  uint8_t *buf;
  uint32_t len;
  uint32_t cap;
  int error; /* non-zero once an allocation failed */
} pb_writer;

void pb_writer_init(pb_writer *w);
void pb_writer_free(pb_writer *w);

/* Detach the buffer to the caller (who then free()s it). Returns the buffer and
 * writes its byte length to *len; the writer is reset to empty. NULL on error.
 */
uint8_t *pb_writer_take(pb_writer *w, uint32_t *len);

/* Field appenders. Each returns 0 on success, -1 on OOM (setting w->error).
 * The *_signed / *_sint / *_float / *_double / *_bool forms are typed sugar
 * over the raw varint/fixed appenders. */
int pb_write_varint(pb_writer *w, uint32_t field, uint64_t v);
int pb_write_signed(pb_writer *w, uint32_t field, int64_t v); /* int32/64: 2's-complement varint */
int pb_write_sint(pb_writer *w, uint32_t field, int64_t v);   /* sint32/64: zigzag varint */
int pb_write_bool(pb_writer *w, uint32_t field, int b);
int pb_write_i32(pb_writer *w, uint32_t field, uint32_t v);
int pb_write_i64(pb_writer *w, uint32_t field, uint64_t v);
int pb_write_float(pb_writer *w, uint32_t field, float f);
int pb_write_double(pb_writer *w, uint32_t field, double d);
int pb_write_bytes(pb_writer *w, uint32_t field, const void *data, uint32_t n);
int pb_write_string(pb_writer *w, uint32_t field, const char *s); /* strlen(s) bytes */

/* Embed `msg` as a length-delimited field (its whole buffer becomes the value).
 * Encode a nested message into its own pb_writer, then embed it here. 0 / -1.
 */
int pb_write_message(pb_writer *w, uint32_t field, const pb_writer *msg);

/* Raw appenders (no field tag) — for hand-building packed repeated payloads:
 * write the elements into a scratch writer with these, then pb_write_bytes it.
 * Each returns 0 / -1. */
int pb_put_varint(pb_writer *w, uint64_t v);
int pb_put_i32(pb_writer *w, uint32_t v);
int pb_put_i64(pb_writer *w, uint64_t v);

#endif /* OWOS_PROTOBUF_H */
