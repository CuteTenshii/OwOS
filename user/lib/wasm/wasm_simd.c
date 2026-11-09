/* wasm_simd.c: the 128-bit SIMD operators (0xFD prefix). A v128 occupies two stack slots, low half first. */
#include "owos.h"
#include "wasm.h"
#include "wasm_int.h"

typedef union {
  uint8_t u8[16];
  int8_t i8[16];
  uint16_t u16[8];
  int16_t i16[8];
  uint32_t u32[4];
  int32_t i32[4];
  uint64_t u64[2];
  int64_t i64[2];
  float f32[4];
  double f64[2];
} v128;

static v128 ld(const uint64_t *s)
{
  v128 v;
  memcpy(&v, s, 16);
  return v;
}

static void st(uint64_t *s, v128 v) { memcpy(s, &v, 16); }

static unsigned popcnt_u8(unsigned x)
{
  x = x - ((x >> 1) & 0x55);
  x = (x & 0x33) + ((x >> 2) & 0x33);
  return (x + (x >> 4)) & 0x0f;
}

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }

#define EACH(n) for (i = 0; i < (n); i++)

/* Lane-wise helpers: r.F[i] = expr over x = a.F[i], y = b.F[i] (for the binary forms) */
#define MAP1(F, n, expr)                                                                                               \
  EACH(n)                                                                                                              \
  {                                                                                                                    \
    __typeof__(a.F[0]) x = a.F[i];                                                                                     \
    r.F[i] = (expr);                                                                                                   \
  }                                                                                                                    \
  break
#define MAP2(F, n, expr)                                                                                               \
  EACH(n)                                                                                                              \
  {                                                                                                                    \
    __typeof__(a.F[0]) x = a.F[i], y = b.F[i];                                                                         \
    r.F[i] = (expr);                                                                                                   \
  }                                                                                                                    \
  break

const char *wasm_simd_exec(unsigned sub, uint64_t **spp, const uint32_t **pcp, wasm_memory *mem)
{
  uint64_t *sp = *spp;
  const uint32_t *pc = *pcp;
  v128 a, b, c, r;
  unsigned i;
  uint64_t ea;
  uint32_t cnt = 0;
  memset(&r, 0, sizeof r);

#define MEMCHK(sz)                                                                                                     \
  ea = (uint64_t)(uint32_t)sp[-1] + *pc++;                                                                             \
  if (ea + (sz) > mem->size)                                                                                           \
  return "memory access out of bounds"

  switch (sub) {
  /* loads */
  case 0x00:
    MEMCHK(16);
    memcpy(&r, mem->data + ea, 16);
    sp[-1] = r.u64[0];
    *sp++ = r.u64[1];
    break;
  case 0x01:
  case 0x02:
  case 0x03:
  case 0x04:
  case 0x05:
  case 0x06: {
    uint8_t src[8];
    MEMCHK(8);
    memcpy(src, mem->data + ea, 8);
    if (sub <= 0x02)
      EACH(8) r.i16[i] = sub == 1 ? (int16_t)(int8_t)src[i] : (int16_t)src[i];
    else if (sub <= 0x04) {
      uint16_t s16[4];
      memcpy(s16, src, 8);
      EACH(4) r.i32[i] = sub == 3 ? (int32_t)(int16_t)s16[i] : (int32_t)s16[i];
    } else {
      uint32_t s32[2];
      memcpy(s32, src, 8);
      EACH(2) r.i64[i] = sub == 5 ? (int64_t)(int32_t)s32[i] : (int64_t)s32[i];
    }
    sp[-1] = r.u64[0];
    *sp++ = r.u64[1];
    break;
  }
  case 0x07:
  case 0x08:
  case 0x09:
  case 0x0a: {
    unsigned sz = 1u << (sub - 7);
    uint8_t src[8];
    MEMCHK(sz);
    memcpy(src, mem->data + ea, sz);
    EACH(16) r.u8[i] = src[i % sz];
    sp[-1] = r.u64[0];
    *sp++ = r.u64[1];
    break;
  }
  case 0x5c:
  case 0x5d: {
    unsigned sz = sub == 0x5c ? 4 : 8;
    MEMCHK(sz);
    memcpy(&r, mem->data + ea, sz);
    sp[-1] = r.u64[0];
    *sp++ = r.u64[1];
    break;
  }
  case 0x0b:
    c = ld(sp - 2);
    sp -= 2;
    MEMCHK(16);
    memcpy(mem->data + ea, &c, 16);
    sp--;
    break;
  case 0x54:
  case 0x55:
  case 0x56:
  case 0x57: {
    unsigned sz = 1u << (sub - 0x54);
    uint32_t lane;
    c = ld(sp - 2);
    sp -= 2;
    lane = pc[1];
    ea = (uint64_t)(uint32_t)sp[-1] + pc[0];
    pc += 2;
    if (ea + sz > mem->size)
      return "memory access out of bounds";
    memcpy(c.u8 + lane * sz, mem->data + ea, sz);
    sp[-1] = c.u64[0];
    *sp++ = c.u64[1];
    break;
  }
  case 0x58:
  case 0x59:
  case 0x5a:
  case 0x5b: {
    unsigned sz = 1u << (sub - 0x58);
    uint32_t lane;
    c = ld(sp - 2);
    sp -= 2;
    lane = pc[1];
    ea = (uint64_t)(uint32_t)sp[-1] + pc[0];
    pc += 2;
    if (ea + sz > mem->size)
      return "memory access out of bounds";
    memcpy(mem->data + ea, c.u8 + lane * sz, sz);
    sp--;
    break;
  }
  case 0x0c:
    sp[0] = (uint64_t)pc[0] | ((uint64_t)pc[1] << 32);
    sp[1] = (uint64_t)pc[2] | ((uint64_t)pc[3] << 32);
    sp += 2;
    pc += 4;
    break;
  case 0x0d: {
    uint8_t lane[16];
    memcpy(lane, pc, 16);
    pc += 4;
    b = ld(sp - 2);
    a = ld(sp - 4);
    EACH(16) r.u8[i] = lane[i] < 16 ? a.u8[lane[i]] : b.u8[lane[i] - 16];
    sp -= 2;
    st(sp - 2, r);
    break;
  }
  case 0x0e:
    b = ld(sp - 2);
    a = ld(sp - 4);
    EACH(16) r.u8[i] = b.u8[i] < 16 ? a.u8[b.u8[i]] : 0;
    sp -= 2;
    st(sp - 2, r);
    break;

  /* splat */
  case 0x0f:
    EACH(16) r.u8[i] = (uint8_t)sp[-1];
    sp++;
    st(sp - 2, r);
    break;
  case 0x10:
    EACH(8) r.u16[i] = (uint16_t)sp[-1];
    sp++;
    st(sp - 2, r);
    break;
  case 0x11:
  case 0x13:
    EACH(4) r.u32[i] = (uint32_t)sp[-1];
    sp++;
    st(sp - 2, r);
    break;
  case 0x12:
  case 0x14:
    r.u64[0] = r.u64[1] = sp[-1];
    sp++;
    st(sp - 2, r);
    break;

  /* lanes */
  case 0x15:
  case 0x16:
  case 0x18:
  case 0x19:
  case 0x1b:
  case 0x1d:
  case 0x1f:
  case 0x21:
    a = ld(sp - 2);
    cnt = *pc++;
    sp -= 1;
    switch (sub) {
    case 0x15:
      sp[-1] = (uint32_t)(int32_t)a.i8[cnt];
      break;
    case 0x16:
      sp[-1] = a.u8[cnt];
      break;
    case 0x18:
      sp[-1] = (uint32_t)(int32_t)a.i16[cnt];
      break;
    case 0x19:
      sp[-1] = a.u16[cnt];
      break;
    case 0x1b:
    case 0x1f:
      sp[-1] = a.u32[cnt];
      break;
    default:
      sp[-1] = a.u64[cnt];
    }
    break;
  case 0x17:
  case 0x1a:
  case 0x1c:
  case 0x1e:
  case 0x20:
  case 0x22: {
    uint64_t v = sp[-1];
    a = ld(sp - 3);
    cnt = *pc++;
    sp--;
    switch (sub) {
    case 0x17:
      a.u8[cnt] = (uint8_t)v;
      break;
    case 0x1a:
      a.u16[cnt] = (uint16_t)v;
      break;
    case 0x1c:
    case 0x20:
      a.u32[cnt] = (uint32_t)v;
      break;
    default:
      a.u64[cnt] = v;
    }
    sp -= 2;
    st(sp, a);
    sp += 2;
    break;
  }

  /* comparisons */
  case 0x23:
  case 0x24:
  case 0x25:
  case 0x26:
  case 0x27:
  case 0x28:
  case 0x29:
  case 0x2a:
  case 0x2b:
  case 0x2c: {
    unsigned k = sub - 0x23;
    b = ld(sp - 2);
    a = ld(sp - 4);
    EACH(16)
    {
      int8_t sx = a.i8[i], sy = b.i8[i];
      uint8_t ux = a.u8[i], uy = b.u8[i];
      int t = k == 0   ? ux == uy
              : k == 1 ? ux != uy
              : k == 2 ? sx < sy
              : k == 3 ? ux < uy
              : k == 4 ? sx > sy
              : k == 5 ? ux > uy
              : k == 6 ? sx <= sy
              : k == 7 ? ux <= uy
              : k == 8 ? sx >= sy
                       : ux >= uy;
      r.i8[i] = t ? -1 : 0;
    }
    sp -= 2;
    st(sp - 2, r);
    break;
  }
  case 0x2d:
  case 0x2e:
  case 0x2f:
  case 0x30:
  case 0x31:
  case 0x32:
  case 0x33:
  case 0x34:
  case 0x35:
  case 0x36: {
    unsigned k = sub - 0x2d;
    b = ld(sp - 2);
    a = ld(sp - 4);
    EACH(8)
    {
      int16_t sx = a.i16[i], sy = b.i16[i];
      uint16_t ux = a.u16[i], uy = b.u16[i];
      int t = k == 0   ? ux == uy
              : k == 1 ? ux != uy
              : k == 2 ? sx < sy
              : k == 3 ? ux < uy
              : k == 4 ? sx > sy
              : k == 5 ? ux > uy
              : k == 6 ? sx <= sy
              : k == 7 ? ux <= uy
              : k == 8 ? sx >= sy
                       : ux >= uy;
      r.i16[i] = t ? -1 : 0;
    }
    sp -= 2;
    st(sp - 2, r);
    break;
  }
  case 0x37:
  case 0x38:
  case 0x39:
  case 0x3a:
  case 0x3b:
  case 0x3c:
  case 0x3d:
  case 0x3e:
  case 0x3f:
  case 0x40: {
    unsigned k = sub - 0x37;
    b = ld(sp - 2);
    a = ld(sp - 4);
    EACH(4)
    {
      int32_t sx = a.i32[i], sy = b.i32[i];
      uint32_t ux = a.u32[i], uy = b.u32[i];
      int t = k == 0   ? ux == uy
              : k == 1 ? ux != uy
              : k == 2 ? sx < sy
              : k == 3 ? ux < uy
              : k == 4 ? sx > sy
              : k == 5 ? ux > uy
              : k == 6 ? sx <= sy
              : k == 7 ? ux <= uy
              : k == 8 ? sx >= sy
                       : ux >= uy;
      r.i32[i] = t ? -1 : 0;
    }
    sp -= 2;
    st(sp - 2, r);
    break;
  }
  case 0x41:
  case 0x42:
  case 0x43:
  case 0x44:
  case 0x45:
  case 0x46: {
    unsigned k = sub - 0x41;
    b = ld(sp - 2);
    a = ld(sp - 4);
    EACH(4)
    {
      float x = a.f32[i], y = b.f32[i];
      int t = k == 0 ? x == y : k == 1 ? x != y : k == 2 ? x < y : k == 3 ? x > y : k == 4 ? x <= y : x >= y;
      r.i32[i] = t ? -1 : 0;
    }
    sp -= 2;
    st(sp - 2, r);
    break;
  }
  case 0x47:
  case 0x48:
  case 0x49:
  case 0x4a:
  case 0x4b:
  case 0x4c: {
    unsigned k = sub - 0x47;
    b = ld(sp - 2);
    a = ld(sp - 4);
    EACH(2)
    {
      double x = a.f64[i], y = b.f64[i];
      int t = k == 0 ? x == y : k == 1 ? x != y : k == 2 ? x < y : k == 3 ? x > y : k == 4 ? x <= y : x >= y;
      r.i64[i] = t ? -1 : 0;
    }
    sp -= 2;
    st(sp - 2, r);
    break;
  }

  /* bitwise */
  case 0x4d:
    a = ld(sp - 2);
    r.u64[0] = ~a.u64[0];
    r.u64[1] = ~a.u64[1];
    st(sp - 2, r);
    break;
  case 0x4e:
  case 0x4f:
  case 0x50:
  case 0x51:
    b = ld(sp - 2);
    a = ld(sp - 4);
    EACH(2)
    r.u64[i] = sub == 0x4e   ? a.u64[i] & b.u64[i]
               : sub == 0x4f ? a.u64[i] & ~b.u64[i]
               : sub == 0x50 ? a.u64[i] | b.u64[i]
                             : a.u64[i] ^ b.u64[i];
    sp -= 2;
    st(sp - 2, r);
    break;
  case 0x52:
    c = ld(sp - 2);
    b = ld(sp - 4);
    a = ld(sp - 6);
    EACH(2) r.u64[i] = (a.u64[i] & c.u64[i]) | (b.u64[i] & ~c.u64[i]);
    sp -= 4;
    st(sp - 2, r);
    break;
  case 0x53:
    a = ld(sp - 2);
    sp--;
    sp[-1] = (a.u64[0] | a.u64[1]) != 0;
    break;

  /* conversions between shapes */
  case 0x5e:
    a = ld(sp - 2);
    r.f32[0] = (float)a.f64[0];
    r.f32[1] = (float)a.f64[1];
    st(sp - 2, r);
    break;
  case 0x5f:
    a = ld(sp - 2);
    r.f64[0] = (double)a.f32[0];
    r.f64[1] = (double)a.f32[1];
    st(sp - 2, r);
    break;
  case 0xf8:
  case 0xf9:
    a = ld(sp - 2);
    EACH(4) r.u32[i] = sub == 0xf8 ? wasm_sat_i32s((double)a.f32[i]) : wasm_sat_i32u((double)a.f32[i]);
    st(sp - 2, r);
    break;
  case 0xfa:
  case 0xfb:
    a = ld(sp - 2);
    EACH(4) r.f32[i] = sub == 0xfa ? (float)a.i32[i] : (float)a.u32[i];
    st(sp - 2, r);
    break;
  case 0xfc:
  case 0xfd:
    a = ld(sp - 2);
    EACH(2) r.u32[i] = sub == 0xfc ? wasm_sat_i32s(a.f64[i]) : wasm_sat_i32u(a.f64[i]);
    st(sp - 2, r);
    break;
  case 0xfe:
  case 0xff:
    a = ld(sp - 2);
    EACH(2) r.f64[i] = sub == 0xfe ? (double)a.i32[i] : (double)a.u32[i];
    st(sp - 2, r);
    break;

  default: {
    /* the remaining operators are lane-wise on the top one or two vectors */
    int binary = 0, shift = 0, test = 0;
    switch (sub) {
    case 0x63:
    case 0x64:
    case 0x83:
    case 0x84:
    case 0xa3:
    case 0xa4:
    case 0xc3:
    case 0xc4:
      test = 1;
      break;
    case 0x6b:
    case 0x6c:
    case 0x6d:
    case 0x8b:
    case 0x8c:
    case 0x8d:
    case 0xab:
    case 0xac:
    case 0xad:
    case 0xcb:
    case 0xcc:
    case 0xcd:
      shift = 1;
      break;
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x67:
    case 0x68:
    case 0x69:
    case 0x6a:
    case 0x74:
    case 0x75:
    case 0x7a:
    case 0x7c:
    case 0x7d:
    case 0x7e:
    case 0x7f:
    case 0x80:
    case 0x81:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x94:
    case 0xa0:
    case 0xa1:
    case 0xa7:
    case 0xa8:
    case 0xa9:
    case 0xaa:
    case 0xc0:
    case 0xc1:
    case 0xc7:
    case 0xc8:
    case 0xc9:
    case 0xca:
    case 0xe0:
    case 0xe1:
    case 0xe3:
    case 0xec:
    case 0xed:
    case 0xef:
      break;
    default:
      binary = 1;
    }
    if (shift) {
      cnt = (uint32_t)sp[-1];
      sp--;
      a = ld(sp - 2);
    } else if (binary) {
      b = ld(sp - 2);
      a = ld(sp - 4);
      sp -= 2;
    } else
      a = ld(sp - 2);
    if (test) {
      uint32_t o = 0;
      switch (sub) {
      case 0x63:
        o = 1;
        EACH(16) if (!a.u8[i]) o = 0;
        break;
      case 0x64:
        EACH(16) o |= (uint32_t)(a.u8[i] >> 7) << i;
        break;
      case 0x83:
        o = 1;
        EACH(8) if (!a.u16[i]) o = 0;
        break;
      case 0x84:
        EACH(8) o |= (uint32_t)(a.u16[i] >> 15) << i;
        break;
      case 0xa3:
        o = 1;
        EACH(4) if (!a.u32[i]) o = 0;
        break;
      case 0xa4:
        EACH(4) o |= (a.u32[i] >> 31) << i;
        break;
      case 0xc3:
        o = a.u64[0] && a.u64[1];
        break;
      default:
        o = (uint32_t)(a.u64[0] >> 63) | (uint32_t)((a.u64[1] >> 63) << 1);
      }
      sp--;
      sp[-1] = o;
      break;
    }
    switch (sub) {
    /* i8x16 */
    case 0x60:
      MAP1(i8, 16, (int8_t)(x < 0 ? -x : x));
    case 0x61:
      MAP1(i8, 16, (int8_t)-x);
    case 0x62:
      MAP1(u8, 16, (uint8_t)popcnt_u8(x));
    case 0x65:
    case 0x66:
      EACH(8)
      {
        r.i8[i] = (int8_t)(sub == 0x65 ? clamp64(a.i16[i], -128, 127) : clamp64(a.i16[i], 0, 255));
        r.i8[8 + i] = (int8_t)(sub == 0x65 ? clamp64(b.i16[i], -128, 127) : clamp64(b.i16[i], 0, 255));
      }
      break;
    case 0x67:
      MAP1(f32, 4, (float)ceil((double)x));
    case 0x68:
      MAP1(f32, 4, (float)floor((double)x));
    case 0x69:
      MAP1(f32, 4, (float)trunc((double)x));
    case 0x6a:
      MAP1(f32, 4, (float)rint((double)x));
    case 0x6b:
      MAP1(u8, 16, (uint8_t)(x << (cnt & 7)));
    case 0x6c:
      MAP1(i8, 16, (int8_t)(x >> (cnt & 7)));
    case 0x6d:
      MAP1(u8, 16, (uint8_t)(x >> (cnt & 7)));
    case 0x6e:
      MAP2(u8, 16, (uint8_t)(x + y));
    case 0x6f:
      MAP2(i8, 16, (int8_t)clamp64((int)x + y, -128, 127));
    case 0x70:
      MAP2(u8, 16, (uint8_t)clamp64((int)x + y, 0, 255));
    case 0x71:
      MAP2(u8, 16, (uint8_t)(x - y));
    case 0x72:
      MAP2(i8, 16, (int8_t)clamp64((int)x - y, -128, 127));
    case 0x73:
      MAP2(u8, 16, (uint8_t)clamp64((int)x - y, 0, 255));
    case 0x74:
      MAP1(f64, 2, ceil(x));
    case 0x75:
      MAP1(f64, 2, floor(x));
    case 0x76:
      MAP2(i8, 16, x < y ? x : y);
    case 0x77:
      MAP2(u8, 16, x < y ? x : y);
    case 0x78:
      MAP2(i8, 16, x > y ? x : y);
    case 0x79:
      MAP2(u8, 16, x > y ? x : y);
    case 0x7a:
      MAP1(f64, 2, trunc(x));
    case 0x7b:
      MAP2(u8, 16, (uint8_t)(((int)x + y + 1) >> 1));
    case 0x7c:
      EACH(8) r.i16[i] = (int16_t)(a.i8[2 * i] + a.i8[2 * i + 1]);
      break;
    case 0x7d:
      EACH(8) r.u16[i] = (uint16_t)(a.u8[2 * i] + a.u8[2 * i + 1]);
      break;
    case 0x7e:
      EACH(4) r.i32[i] = a.i16[2 * i] + a.i16[2 * i + 1];
      break;
    case 0x7f:
      EACH(4) r.u32[i] = (uint32_t)a.u16[2 * i] + a.u16[2 * i + 1];
      break;
    /* i16x8 */
    case 0x80:
      MAP1(i16, 8, (int16_t)(x < 0 ? -x : x));
    case 0x81:
      MAP1(i16, 8, (int16_t)-x);
    case 0x82:
      MAP2(i16, 8, (int16_t)clamp64((((int32_t)x * y) + 0x4000) >> 15, -32768, 32767));
    case 0x85:
    case 0x86:
      EACH(4)
      {
        r.i16[i] = (int16_t)(sub == 0x85 ? clamp64(a.i32[i], -32768, 32767) : clamp64(a.i32[i], 0, 65535));
        r.i16[4 + i] = (int16_t)(sub == 0x85 ? clamp64(b.i32[i], -32768, 32767) : clamp64(b.i32[i], 0, 65535));
      }
      break;
    case 0x87:
      EACH(8) r.i16[i] = a.i8[i];
      break;
    case 0x88:
      EACH(8) r.i16[i] = a.i8[8 + i];
      break;
    case 0x89:
      EACH(8) r.u16[i] = a.u8[i];
      break;
    case 0x8a:
      EACH(8) r.u16[i] = a.u8[8 + i];
      break;
    case 0x8b:
      MAP1(u16, 8, (uint16_t)(x << (cnt & 15)));
    case 0x8c:
      MAP1(i16, 8, (int16_t)(x >> (cnt & 15)));
    case 0x8d:
      MAP1(u16, 8, (uint16_t)(x >> (cnt & 15)));
    case 0x8e:
      MAP2(u16, 8, (uint16_t)(x + y));
    case 0x8f:
      MAP2(i16, 8, (int16_t)clamp64((int)x + y, -32768, 32767));
    case 0x90:
      MAP2(u16, 8, (uint16_t)clamp64((int)x + y, 0, 65535));
    case 0x91:
      MAP2(u16, 8, (uint16_t)(x - y));
    case 0x92:
      MAP2(i16, 8, (int16_t)clamp64((int)x - y, -32768, 32767));
    case 0x93:
      MAP2(u16, 8, (uint16_t)clamp64((int)x - y, 0, 65535));
    case 0x94:
      MAP1(f64, 2, rint(x));
    case 0x95:
      MAP2(u16, 8, (uint16_t)((uint32_t)x * y));
    case 0x96:
      MAP2(i16, 8, x < y ? x : y);
    case 0x97:
      MAP2(u16, 8, x < y ? x : y);
    case 0x98:
      MAP2(i16, 8, x > y ? x : y);
    case 0x99:
      MAP2(u16, 8, x > y ? x : y);
    case 0x9b:
      MAP2(u16, 8, (uint16_t)(((uint32_t)x + y + 1) >> 1));
    case 0x9c:
      EACH(8) r.i16[i] = (int16_t)(a.i8[i] * b.i8[i]);
      break;
    case 0x9d:
      EACH(8) r.i16[i] = (int16_t)(a.i8[8 + i] * b.i8[8 + i]);
      break;
    case 0x9e:
      EACH(8) r.u16[i] = (uint16_t)(a.u8[i] * b.u8[i]);
      break;
    case 0x9f:
      EACH(8) r.u16[i] = (uint16_t)(a.u8[8 + i] * b.u8[8 + i]);
      break;
    /* i32x4 */
    case 0xa0:
      MAP1(i32, 4, x < 0 ? (int32_t)(0u - (uint32_t)x) : x);
    case 0xa1:
      MAP1(i32, 4, (int32_t)(0u - (uint32_t)x));
    case 0xa7:
      EACH(4) r.i32[i] = a.i16[i];
      break;
    case 0xa8:
      EACH(4) r.i32[i] = a.i16[4 + i];
      break;
    case 0xa9:
      EACH(4) r.u32[i] = a.u16[i];
      break;
    case 0xaa:
      EACH(4) r.u32[i] = a.u16[4 + i];
      break;
    case 0xab:
      MAP1(u32, 4, x << (cnt & 31));
    case 0xac:
      MAP1(i32, 4, x >> (cnt & 31));
    case 0xad:
      MAP1(u32, 4, x >> (cnt & 31));
    case 0xae:
      MAP2(u32, 4, x + y);
    case 0xb1:
      MAP2(u32, 4, x - y);
    case 0xb5:
      MAP2(u32, 4, x * y);
    case 0xb6:
      MAP2(i32, 4, x < y ? x : y);
    case 0xb7:
      MAP2(u32, 4, x < y ? x : y);
    case 0xb8:
      MAP2(i32, 4, x > y ? x : y);
    case 0xb9:
      MAP2(u32, 4, x > y ? x : y);
    case 0xba:
      EACH(4)
      r.u32[i] =
          (uint32_t)((int32_t)a.i16[2 * i] * b.i16[2 * i]) + (uint32_t)((int32_t)a.i16[2 * i + 1] * b.i16[2 * i + 1]);
      break;
    case 0xbc:
      EACH(4) r.i32[i] = (int32_t)a.i16[i] * b.i16[i];
      break;
    case 0xbd:
      EACH(4) r.i32[i] = (int32_t)a.i16[4 + i] * b.i16[4 + i];
      break;
    case 0xbe:
      EACH(4) r.u32[i] = (uint32_t)a.u16[i] * b.u16[i];
      break;
    case 0xbf:
      EACH(4) r.u32[i] = (uint32_t)a.u16[4 + i] * b.u16[4 + i];
      break;
    /* i64x2 */
    case 0xc0:
      MAP1(i64, 2, x < 0 ? (int64_t)(0ull - (uint64_t)x) : x);
    case 0xc1:
      MAP1(i64, 2, (int64_t)(0ull - (uint64_t)x));
    case 0xc7:
      EACH(2) r.i64[i] = a.i32[i];
      break;
    case 0xc8:
      EACH(2) r.i64[i] = a.i32[2 + i];
      break;
    case 0xc9:
      EACH(2) r.u64[i] = a.u32[i];
      break;
    case 0xca:
      EACH(2) r.u64[i] = a.u32[2 + i];
      break;
    case 0xcb:
      MAP1(u64, 2, x << (cnt & 63));
    case 0xcc:
      MAP1(i64, 2, x >> (cnt & 63));
    case 0xcd:
      MAP1(u64, 2, x >> (cnt & 63));
    case 0xce:
      MAP2(u64, 2, x + y);
    case 0xd1:
      MAP2(u64, 2, x - y);
    case 0xd5:
      MAP2(u64, 2, x * y);
    case 0xd6:
      MAP2(i64, 2, x == y ? -1 : 0);
    case 0xd7:
      MAP2(i64, 2, x != y ? -1 : 0);
    case 0xd8:
      MAP2(i64, 2, x < y ? -1 : 0);
    case 0xd9:
      MAP2(i64, 2, x > y ? -1 : 0);
    case 0xda:
      MAP2(i64, 2, x <= y ? -1 : 0);
    case 0xdb:
      MAP2(i64, 2, x >= y ? -1 : 0);
    case 0xdc:
      EACH(2) r.i64[i] = (int64_t)a.i32[i] * b.i32[i];
      break;
    case 0xdd:
      EACH(2) r.i64[i] = (int64_t)a.i32[2 + i] * b.i32[2 + i];
      break;
    case 0xde:
      EACH(2) r.u64[i] = (uint64_t)a.u32[i] * b.u32[i];
      break;
    case 0xdf:
      EACH(2) r.u64[i] = (uint64_t)a.u32[2 + i] * b.u32[2 + i];
      break;
    /* f32x4 */
    case 0xe0:
      EACH(4) r.u32[i] = a.u32[i] & 0x7fffffffu;
      break;
    case 0xe1:
      EACH(4) r.u32[i] = a.u32[i] ^ 0x80000000u;
      break;
    case 0xe3:
      MAP1(f32, 4, (float)sqrt((double)x));
    case 0xe4:
      MAP2(f32, 4, x + y);
    case 0xe5:
      MAP2(f32, 4, x - y);
    case 0xe6:
      MAP2(f32, 4, x * y);
    case 0xe7:
      MAP2(f32, 4, x / y);
    case 0xe8:
      MAP2(f32, 4, wasm_f32_min(x, y));
    case 0xe9:
      MAP2(f32, 4, wasm_f32_max(x, y));
    case 0xea:
      MAP2(f32, 4, y < x ? y : x);
    case 0xeb:
      MAP2(f32, 4, x < y ? y : x);
    /* f64x2 */
    case 0xec:
      EACH(2) r.u64[i] = a.u64[i] & 0x7fffffffffffffffull;
      break;
    case 0xed:
      EACH(2) r.u64[i] = a.u64[i] ^ 0x8000000000000000ull;
      break;
    case 0xef:
      MAP1(f64, 2, sqrt(x));
    case 0xf0:
      MAP2(f64, 2, x + y);
    case 0xf1:
      MAP2(f64, 2, x - y);
    case 0xf2:
      MAP2(f64, 2, x * y);
    case 0xf3:
      MAP2(f64, 2, x / y);
    case 0xf4:
      MAP2(f64, 2, wasm_f64_min(x, y));
    case 0xf5:
      MAP2(f64, 2, wasm_f64_max(x, y));
    case 0xf6:
      MAP2(f64, 2, y < x ? y : x);
    case 0xf7:
      MAP2(f64, 2, x < y ? y : x);
    default:
      return "invalid SIMD instruction";
    }
    st(sp - 2, r);
    break;
  }
  }
  *spp = sp;
  *pcp = pc;
  return NULL;
}
