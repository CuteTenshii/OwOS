/* wasm_int.h: internal bytecode shared by the compiler (wasm.c) and the interpreter (wasm_run.c).
 *
 * The compiler turns each function body into an array of 32-bit words: an opcode followed by its immediates.
 * Single-byte WebAssembly opcodes keep their value; 0xFC and 0xFD prefixed ones become OP_FC(n) and OP_FD(n);
 * control flow is lowered to the OP_* forms below with branch targets resolved to word offsets.
 *
 * Branches that must discard operands carry (target, arity, dest): `arity` slots are copied to fp[dest]. */
#ifndef OWOS_WASM_INT_H
#define OWOS_WASM_INT_H

enum {
  OP_JMP = 0x100, /* target                                   */
  OP_JMP_IF,      /* target; pops the condition               */
  OP_JMP_UNLESS,  /* target; pops the condition               */
  OP_BR,          /* target, arity, dest                      */
  OP_BR_IF,       /* target, arity, dest; pops the condition  */
  OP_BR_TABLE,    /* n, then n + 1 of (target, arity, dest)   */
  OP_RETURN,
  OP_DROP2,
  OP_SELECT2,
  OP_LOCAL_GET2, /* slot */
  OP_LOCAL_SET2,
  OP_LOCAL_TEE2,
  OP_GLOBAL_GET2, /* index */
  OP_GLOBAL_SET2,
  OP_GLOBAL_GET_X, /* externref global */
  OP_GLOBAL_SET_X
};

#define OP_FC(n) (0x200 + (n))
#define OP_FD(n) (0x300 + (n))
#define WASM_OP_LIMIT 0x400

#include "math.h"
#include "wasm.h"

/* Floating point helpers shared by the interpreter and the SIMD operators. Slots hold raw bits. */
static inline float f32_of(uint64_t v)
{
  float f;
  uint32_t u = (uint32_t)v;
  memcpy(&f, &u, 4);
  return f;
}
static inline uint64_t of_f32(float f)
{
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}
static inline double f64_of(uint64_t v)
{
  double d;
  memcpy(&d, &v, 8);
  return d;
}
static inline uint64_t of_f64(double d)
{
  uint64_t u;
  memcpy(&u, &d, 8);
  return u;
}

static inline float wasm_f32_min(float a, float b)
{
  if (a != a || b != b)
    return a + b;
  if (a == 0 && b == 0)
    return (of_f32(a) >> 31) ? a : b;
  return a < b ? a : b;
}
static inline float wasm_f32_max(float a, float b)
{
  if (a != a || b != b)
    return a + b;
  if (a == 0 && b == 0)
    return (of_f32(a) >> 31) ? b : a;
  return a > b ? a : b;
}
static inline double wasm_f64_min(double a, double b)
{
  if (a != a || b != b)
    return a + b;
  if (a == 0 && b == 0)
    return (of_f64(a) >> 63) ? a : b;
  return a < b ? a : b;
}
static inline double wasm_f64_max(double a, double b)
{
  if (a != a || b != b)
    return a + b;
  if (a == 0 && b == 0)
    return (of_f64(a) >> 63) ? b : a;
  return a > b ? a : b;
}

/* Truncating conversions: 0 when the value is out of range or NaN */
static inline int wasm_tr_i32s(double d, uint32_t *o)
{
  if (!(d > -2147483649.0 && d < 2147483648.0))
    return 0;
  *o = (uint32_t)(int32_t)d;
  return 1;
}
static inline int wasm_tr_i32u(double d, uint32_t *o)
{
  if (!(d > -1.0 && d < 4294967296.0))
    return 0;
  *o = (uint32_t)(int64_t)d;
  return 1;
}
static inline int wasm_tr_i64s(double d, uint64_t *o)
{
  if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
    return 0;
  *o = (uint64_t)(int64_t)d;
  return 1;
}
static inline int wasm_tr_i64u(double d, uint64_t *o)
{
  if (!(d > -1.0 && d < 18446744073709551616.0))
    return 0;
  *o = d >= 9223372036854775808.0 ? (uint64_t)(int64_t)(d - 9223372036854775808.0) + ((uint64_t)1 << 63)
                                  : (uint64_t)(int64_t)d;
  return 1;
}
static inline uint32_t wasm_sat_i32s(double d)
{
  uint32_t o;
  if (d != d)
    return 0;
  if (wasm_tr_i32s(d, &o))
    return o;
  return d < 0 ? 0x80000000u : 0x7fffffffu;
}
static inline uint32_t wasm_sat_i32u(double d)
{
  uint32_t o;
  if (d != d)
    return 0;
  if (wasm_tr_i32u(d, &o))
    return o;
  return d < 0 ? 0u : 0xffffffffu;
}
static inline uint64_t wasm_sat_i64s(double d)
{
  uint64_t o;
  if (d != d)
    return 0;
  if (wasm_tr_i64s(d, &o))
    return o;
  return d < 0 ? (uint64_t)1 << 63 : ~((uint64_t)1 << 63);
}
static inline uint64_t wasm_sat_i64u(double d)
{
  uint64_t o;
  if (d != d)
    return 0;
  if (wasm_tr_i64u(d, &o))
    return o;
  return d < 0 ? 0 : ~(uint64_t)0;
}

/* Execute one 0xFD operator. *sp and *pc advance; returns a trap message or NULL. */
const char *wasm_simd_exec(unsigned sub, uint64_t **sp, const uint32_t **pc, wasm_memory *mem);

#endif
