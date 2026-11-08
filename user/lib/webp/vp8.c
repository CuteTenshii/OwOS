/* vp8.c — VP8 key-frame decoder for lossy WebP. See vp8.h.
 *
 * Reconstructs the whole frame unfiltered (intra prediction reads unfiltered
 * neighbours), runs the in-loop deblocking filter over it in macroblock order,
 * then converts to RGBA with libwebp's "fancy" bilinear chroma upsampler, so
 * the output matches dwebp's default bit for bit. The probability, quantizer
 * and mode tables are libwebp's.
 */
#include "vp8.h"
#include "owos.h"
#include <stdint.h>

#define BPS 32 /* row stride of the per-macroblock work buffers */

/* Intra 4x4 modes; the 16x16 and chroma modes reuse the first four. */
enum { B_DC_PRED, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_RD_PRED, B_VR_PRED, B_LD_PRED, B_VL_PRED, B_HD_PRED, B_HU_PRED };

static const uint8_t kDcTable[128] = {
    4,   5,   6,   7,   8,   9,   10,  10,  11,  12,  13,  14,  15,  16,  17,  17,  18,  19,  20,  20,  21,  21,
    22,  22,  23,  23,  24,  25,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  37,  38,  39,
    40,  41,  42,  43,  44,  45,  46,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,
    61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  76,  77,  78,  79,  80,  81,
    82,  83,  84,  85,  86,  87,  88,  89,  91,  93,  95,  96,  98,  100, 101, 102, 104, 106, 108, 110, 112, 114,
    116, 118, 122, 124, 126, 128, 130, 132, 134, 136, 138, 140, 143, 145, 148, 151, 154, 157,
};

static const uint16_t kAcTable[128] = {
    4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
    48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  60,  62,  64,  66,  68,  70,  72,  74,  76,  78,  80,
    82,  84,  86,  88,  90,  92,  94,  96,  98,  100, 102, 104, 106, 108, 110, 112, 114, 116, 119, 122, 125, 128,
    131, 134, 137, 140, 143, 146, 149, 152, 155, 158, 161, 164, 167, 170, 173, 177, 181, 185, 189, 193, 197, 201,
    205, 209, 213, 217, 221, 225, 229, 234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284,
};

static const uint8_t kCoeffsUpdate[4 * 8 * 3 * 11] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 176, 246, 255, 255, 255, 255, 255, 255, 255, 255, 255, 223, 241,
    252, 255, 255, 255, 255, 255, 255, 255, 255, 249, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 244, 252,
    255, 255, 255, 255, 255, 255, 255, 255, 234, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 253, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 246, 254, 255, 255, 255, 255, 255, 255, 255, 255, 239, 253, 254, 255, 255,
    255, 255, 255, 255, 255, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 248, 254, 255, 255, 255,
    255, 255, 255, 255, 255, 251, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, 251, 254, 254, 255, 255, 255, 255, 255,
    255, 255, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 253, 255, 254, 255, 255, 255, 255,
    255, 255, 250, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 217, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 225,
    252, 241, 253, 255, 255, 254, 255, 255, 255, 255, 234, 250, 241, 250, 253, 255, 253, 254, 255, 255, 255, 255, 254,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 223, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 238, 253, 254,
    254, 255, 255, 255, 255, 255, 255, 255, 255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255, 249, 254, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 253, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 247, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, 252, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 254, 255, 255, 255, 255, 255,
    255, 255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255, 250, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 186,
    251, 250, 255, 255, 255, 255, 255, 255, 255, 255, 234, 251, 244, 254, 255, 255, 255, 255, 255, 255, 255, 251, 251,
    243, 253, 254, 255, 254, 255, 255, 255, 255, 255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, 236, 253, 254,
    255, 255, 255, 255, 255, 255, 255, 255, 251, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255, 254, 254, 255,
    255, 255, 255, 255, 255, 255, 255, 254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 254, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 248, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 250, 254,
    252, 254, 255, 255, 255, 255, 255, 255, 255, 248, 254, 249, 253, 255, 255, 255, 255, 255, 255, 255, 255, 253, 253,
    255, 255, 255, 255, 255, 255, 255, 255, 246, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255, 252, 254, 251, 254,
    254, 255, 255, 255, 255, 255, 255, 255, 254, 252, 255, 255, 255, 255, 255, 255, 255, 255, 248, 254, 253, 255, 255,
    255, 255, 255, 255, 255, 255, 253, 255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 251, 254, 255, 255, 255,
    255, 255, 255, 255, 255, 245, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255, 253, 253, 254, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 251, 253, 255, 255, 255, 255, 255, 255, 255, 255, 252, 253, 254, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 252, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 249, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 254,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

static const uint8_t kCoeffsDefault[4 * 8 * 3 * 11] = {
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 253, 136, 254, 255, 228, 219, 128, 128, 128, 128, 128, 189, 129,
    242, 255, 227, 213, 255, 219, 128, 128, 128, 106, 126, 227, 252, 214, 209, 255, 255, 128, 128, 128, 1,   98,  248,
    255, 236, 226, 255, 255, 128, 128, 128, 181, 133, 238, 254, 221, 234, 255, 154, 128, 128, 128, 78,  134, 202, 247,
    198, 180, 255, 219, 128, 128, 128, 1,   185, 249, 255, 243, 255, 128, 128, 128, 128, 128, 184, 150, 247, 255, 236,
    224, 128, 128, 128, 128, 128, 77,  110, 216, 255, 236, 230, 128, 128, 128, 128, 128, 1,   101, 251, 255, 241, 255,
    128, 128, 128, 128, 128, 170, 139, 241, 252, 236, 209, 255, 255, 128, 128, 128, 37,  116, 196, 243, 228, 255, 255,
    255, 128, 128, 128, 1,   204, 254, 255, 245, 255, 128, 128, 128, 128, 128, 207, 160, 250, 255, 238, 128, 128, 128,
    128, 128, 128, 102, 103, 231, 255, 211, 171, 128, 128, 128, 128, 128, 1,   152, 252, 255, 240, 255, 128, 128, 128,
    128, 128, 177, 135, 243, 255, 234, 225, 128, 128, 128, 128, 128, 80,  129, 211, 255, 194, 224, 128, 128, 128, 128,
    128, 1,   1,   255, 128, 128, 128, 128, 128, 128, 128, 128, 246, 1,   255, 128, 128, 128, 128, 128, 128, 128, 128,
    255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 198, 35,  237, 223, 193, 187, 162, 160, 145, 155, 62,  131,
    45,  198, 221, 172, 176, 220, 157, 252, 221, 1,   68,  47,  146, 208, 149, 167, 221, 162, 255, 223, 128, 1,   149,
    241, 255, 221, 224, 255, 255, 128, 128, 128, 184, 141, 234, 253, 222, 220, 255, 199, 128, 128, 128, 81,  99,  181,
    242, 176, 190, 249, 202, 255, 255, 128, 1,   129, 232, 253, 214, 197, 242, 196, 255, 255, 128, 99,  121, 210, 250,
    201, 198, 255, 202, 128, 128, 128, 23,  91,  163, 242, 170, 187, 247, 210, 255, 255, 128, 1,   200, 246, 255, 234,
    255, 128, 128, 128, 128, 128, 109, 178, 241, 255, 231, 245, 255, 255, 128, 128, 128, 44,  130, 201, 253, 205, 192,
    255, 255, 128, 128, 128, 1,   132, 239, 251, 219, 209, 255, 165, 128, 128, 128, 94,  136, 225, 251, 218, 190, 255,
    255, 128, 128, 128, 22,  100, 174, 245, 186, 161, 255, 199, 128, 128, 128, 1,   182, 249, 255, 232, 235, 128, 128,
    128, 128, 128, 124, 143, 241, 255, 227, 234, 128, 128, 128, 128, 128, 35,  77,  181, 251, 193, 211, 255, 205, 128,
    128, 128, 1,   157, 247, 255, 236, 231, 255, 255, 128, 128, 128, 121, 141, 235, 255, 225, 227, 255, 255, 128, 128,
    128, 45,  99,  188, 251, 195, 217, 255, 224, 128, 128, 128, 1,   1,   251, 255, 213, 255, 128, 128, 128, 128, 128,
    203, 1,   248, 255, 255, 128, 128, 128, 128, 128, 128, 137, 1,   177, 255, 224, 255, 128, 128, 128, 128, 128, 253,
    9,   248, 251, 207, 208, 255, 192, 128, 128, 128, 175, 13,  224, 243, 193, 185, 249, 198, 255, 255, 128, 73,  17,
    171, 221, 161, 179, 236, 167, 255, 234, 128, 1,   95,  247, 253, 212, 183, 255, 255, 128, 128, 128, 239, 90,  244,
    250, 211, 209, 255, 255, 128, 128, 128, 155, 77,  195, 248, 188, 195, 255, 255, 128, 128, 128, 1,   24,  239, 251,
    218, 219, 255, 205, 128, 128, 128, 201, 51,  219, 255, 196, 186, 128, 128, 128, 128, 128, 69,  46,  190, 239, 201,
    218, 255, 228, 128, 128, 128, 1,   191, 251, 255, 255, 128, 128, 128, 128, 128, 128, 223, 165, 249, 255, 213, 255,
    128, 128, 128, 128, 128, 141, 124, 248, 255, 255, 128, 128, 128, 128, 128, 128, 1,   16,  248, 255, 255, 128, 128,
    128, 128, 128, 128, 190, 36,  230, 255, 236, 255, 128, 128, 128, 128, 128, 149, 1,   255, 128, 128, 128, 128, 128,
    128, 128, 128, 1,   226, 255, 128, 128, 128, 128, 128, 128, 128, 128, 247, 192, 255, 128, 128, 128, 128, 128, 128,
    128, 128, 240, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128, 1,   134, 252, 255, 255, 128, 128, 128, 128, 128,
    128, 213, 62,  250, 255, 255, 128, 128, 128, 128, 128, 128, 55,  93,  255, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 202, 24,  213, 235, 186, 191, 220, 160, 240, 175, 255, 126, 38,
    182, 232, 169, 184, 228, 174, 255, 187, 128, 61,  46,  138, 219, 151, 178, 240, 170, 255, 216, 128, 1,   112, 230,
    250, 199, 191, 247, 159, 255, 255, 128, 166, 109, 228, 252, 211, 215, 255, 174, 128, 128, 128, 39,  77,  162, 232,
    172, 180, 245, 178, 255, 255, 128, 1,   52,  220, 246, 198, 199, 249, 220, 255, 255, 128, 124, 74,  191, 243, 183,
    193, 250, 221, 255, 255, 128, 24,  71,  130, 219, 154, 170, 243, 182, 255, 255, 128, 1,   182, 225, 249, 219, 240,
    255, 224, 128, 128, 128, 149, 150, 226, 252, 216, 205, 255, 171, 128, 128, 128, 28,  108, 170, 242, 183, 194, 254,
    223, 255, 255, 128, 1,   81,  230, 252, 204, 203, 255, 192, 128, 128, 128, 123, 102, 209, 247, 188, 196, 255, 233,
    128, 128, 128, 20,  95,  153, 243, 164, 173, 255, 203, 128, 128, 128, 1,   222, 248, 255, 216, 213, 128, 128, 128,
    128, 128, 168, 175, 246, 252, 235, 205, 255, 255, 128, 128, 128, 47,  116, 215, 255, 211, 212, 255, 255, 128, 128,
    128, 1,   121, 236, 253, 212, 214, 255, 255, 128, 128, 128, 141, 84,  213, 252, 201, 202, 255, 219, 128, 128, 128,
    42,  80,  160, 240, 162, 185, 255, 205, 128, 128, 128, 1,   1,   255, 128, 128, 128, 128, 128, 128, 128, 128, 244,
    1,   255, 128, 128, 128, 128, 128, 128, 128, 128, 238, 1,   255, 128, 128, 128, 128, 128, 128, 128, 128,
};

static const uint8_t kBModesProba[10 * 10 * 9] = {
    231, 120, 48,  89,  115, 113, 120, 152, 112, 152, 179, 64,  126, 170, 118, 46,  70,  95,  175, 69,  143, 80,  85,
    82,  72,  155, 103, 56,  58,  10,  171, 218, 189, 17,  13,  152, 114, 26,  17,  163, 44,  195, 21,  10,  173, 121,
    24,  80,  195, 26,  62,  44,  64,  85,  144, 71,  10,  38,  171, 213, 144, 34,  26,  170, 46,  55,  19,  136, 160,
    33,  206, 71,  63,  20,  8,   114, 114, 208, 12,  9,   226, 81,  40,  11,  96,  182, 84,  29,  16,  36,  134, 183,
    89,  137, 98,  101, 106, 165, 148, 72,  187, 100, 130, 157, 111, 32,  75,  80,  66,  102, 167, 99,  74,  62,  40,
    234, 128, 41,  53,  9,   178, 241, 141, 26,  8,   107, 74,  43,  26,  146, 73,  166, 49,  23,  157, 65,  38,  105,
    160, 51,  52,  31,  115, 128, 104, 79,  12,  27,  217, 255, 87,  17,  7,   87,  68,  71,  44,  114, 51,  15,  186,
    23,  47,  41,  14,  110, 182, 183, 21,  17,  194, 66,  45,  25,  102, 197, 189, 23,  18,  22,  88,  88,  147, 150,
    42,  46,  45,  196, 205, 43,  97,  183, 117, 85,  38,  35,  179, 61,  39,  53,  200, 87,  26,  21,  43,  232, 171,
    56,  34,  51,  104, 114, 102, 29,  93,  77,  39,  28,  85,  171, 58,  165, 90,  98,  64,  34,  22,  116, 206, 23,
    34,  43,  166, 73,  107, 54,  32,  26,  51,  1,   81,  43,  31,  68,  25,  106, 22,  64,  171, 36,  225, 114, 34,
    19,  21,  102, 132, 188, 16,  76,  124, 62,  18,  78,  95,  85,  57,  50,  48,  51,  193, 101, 35,  159, 215, 111,
    89,  46,  111, 60,  148, 31,  172, 219, 228, 21,  18,  111, 112, 113, 77,  85,  179, 255, 38,  120, 114, 40,  42,
    1,   196, 245, 209, 10,  25,  109, 88,  43,  29,  140, 166, 213, 37,  43,  154, 61,  63,  30,  155, 67,  45,  68,
    1,   209, 100, 80,  8,   43,  154, 1,   51,  26,  71,  142, 78,  78,  16,  255, 128, 34,  197, 171, 41,  40,  5,
    102, 211, 183, 4,   1,   221, 51,  50,  17,  168, 209, 192, 23,  25,  82,  138, 31,  36,  171, 27,  166, 38,  44,
    229, 67,  87,  58,  169, 82,  115, 26,  59,  179, 63,  59,  90,  180, 59,  166, 93,  73,  154, 40,  40,  21,  116,
    143, 209, 34,  39,  175, 47,  15,  16,  183, 34,  223, 49,  45,  183, 46,  17,  33,  183, 6,   98,  15,  32,  183,
    57,  46,  22,  24,  128, 1,   54,  17,  37,  65,  32,  73,  115, 28,  128, 23,  128, 205, 40,  3,   9,   115, 51,
    192, 18,  6,   223, 87,  37,  9,   115, 59,  77,  64,  21,  47,  104, 55,  44,  218, 9,   54,  53,  130, 226, 64,
    90,  70,  205, 40,  41,  23,  26,  57,  54,  57,  112, 184, 5,   41,  38,  166, 213, 30,  34,  26,  133, 152, 116,
    10,  32,  134, 39,  19,  53,  221, 26,  114, 32,  73,  255, 31,  9,   65,  234, 2,   15,  1,   118, 73,  75,  32,
    12,  51,  192, 255, 160, 43,  51,  88,  31,  35,  67,  102, 85,  55,  186, 85,  56,  21,  23,  111, 59,  205, 45,
    37,  192, 55,  38,  70,  124, 73,  102, 1,   34,  98,  125, 98,  42,  88,  104, 85,  117, 175, 82,  95,  84,  53,
    89,  128, 100, 113, 101, 45,  75,  79,  123, 47,  51,  128, 81,  171, 1,   57,  17,  5,   71,  102, 57,  53,  41,
    49,  38,  33,  13,  121, 57,  73,  26,  1,   85,  41,  10,  67,  138, 77,  110, 90,  47,  114, 115, 21,  2,   10,
    102, 255, 166, 23,  6,   101, 29,  16,  10,  85,  128, 101, 196, 26,  57,  18,  10,  102, 102, 213, 34,  20,  43,
    117, 20,  15,  36,  163, 128, 68,  1,   26,  102, 61,  71,  37,  34,  53,  31,  243, 192, 69,  60,  71,  38,  73,
    119, 28,  222, 37,  68,  45,  128, 34,  1,   47,  11,  245, 171, 62,  17,  19,  70,  146, 85,  55,  62,  70,  37,
    43,  37,  154, 100, 163, 85,  160, 1,   63,  9,   92,  136, 28,  64,  32,  201, 85,  75,  15,  9,   9,   64,  255,
    184, 119, 16,  86,  6,   28,  5,   64,  255, 25,  248, 1,   56,  8,   17,  132, 137, 255, 55,  116, 128, 58,  15,
    20,  82,  135, 57,  26,  121, 40,  164, 50,  31,  137, 154, 133, 25,  35,  218, 51,  103, 44,  131, 131, 123, 31,
    6,   158, 86,  40,  64,  135, 148, 224, 45,  183, 128, 22,  26,  17,  131, 240, 154, 14,  1,   209, 45,  16,  21,
    91,  64,  222, 7,   1,   197, 56,  21,  39,  155, 60,  138, 23,  102, 213, 83,  12,  13,  54,  192, 255, 68,  47,
    28,  85,  26,  85,  85,  128, 128, 32,  146, 171, 18,  11,  7,   63,  144, 171, 4,   4,   246, 35,  27,  10,  146,
    174, 171, 12,  26,  128, 190, 80,  35,  99,  180, 80,  126, 54,  45,  85,  126, 47,  87,  176, 51,  41,  20,  32,
    101, 75,  128, 139, 118, 146, 116, 128, 85,  56,  41,  15,  176, 236, 85,  37,  9,   62,  71,  30,  17,  119, 118,
    255, 17,  18,  138, 101, 38,  60,  138, 55,  70,  43,  26,  142, 146, 36,  19,  30,  171, 255, 97,  27,  20,  138,
    45,  61,  62,  219, 1,   81,  188, 64,  32,  41,  20,  117, 151, 142, 20,  21,  163, 112, 19,  12,  61,  195, 128,
    48,  4,   24,
};

static const uint8_t kBands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
static const uint8_t kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t kCat3[] = {173, 148, 140, 0};
static const uint8_t kCat4[] = {176, 155, 140, 135, 0};
static const uint8_t kCat5[] = {180, 157, 141, 134, 130, 0};
static const uint8_t kCat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};
static const uint8_t *const kCat3456[4] = {kCat3, kCat4, kCat5, kCat6};

/* Intra 4x4 mode tree: positive entries index the next node, others are -mode. */
static const int8_t kYModesIntra4[18] = {-B_DC_PRED, 1, -B_TM_PRED, 2, -B_VE_PRED, 3,
                                         4,          6, -B_HE_PRED, 5, -B_RD_PRED, -B_VR_PRED,
                                         -B_LD_PRED, 7, -B_VL_PRED, 8, -B_HD_PRED, -B_HU_PRED};

/* ====================================================================== *
 *  Boolean entropy decoder (RFC 6386 section 7)                           *
 * ====================================================================== */

struct bd {
  const uint8_t *p, *end;
  uint32_t value; /* 8 + bits significant bits; the top 8 are the window */
  int range;      /* 128..255                                            */
  int bits;       /* window position within value (< 0: needs a byte)    */
};

static void bd_init(struct bd *b, const uint8_t *p, int size)
{
  b->p = p;
  b->end = p + size;
  b->value = 0;
  b->range = 255;
  b->bits = -8;
}

static int bd_bit(struct bd *b, int prob)
{
  if (b->bits < 0) {
    b->value = (b->value << 8) | (b->p < b->end ? *b->p++ : 0u);
    b->bits += 8;
  }
  int split = 1 + (((b->range - 1) * prob) >> 8);
  uint32_t big = (uint32_t)split << b->bits;
  int bit;
  if (b->value >= big) {
    b->range -= split;
    b->value -= big;
    bit = 1;
  } else {
    b->range = split;
    bit = 0;
  }
  int shift = __builtin_clz((unsigned)b->range) - 24;
  b->range <<= shift;
  b->bits -= shift;
  return bit;
}

static unsigned bd_value(struct bd *b, int nbits)
{
  unsigned v = 0;
  while (nbits-- > 0)
    v |= (unsigned)bd_bit(b, 0x80) << nbits;
  return v;
}

static int bd_signed_value(struct bd *b, int nbits)
{
  int v = (int)bd_value(b, nbits);
  return bd_bit(b, 0x80) ? -v : v;
}

static int bd_sign(struct bd *b, int v) { return bd_bit(b, 0x80) ? -v : v; }

/* ====================================================================== *
 *  Decoder state                                                          *
 * ====================================================================== */

struct nzctx {
  uint8_t y[4], u[2], v[2], dc;
};

struct quant {
  int y1[2], y2[2], uv[2]; /* [0] = DC, [1] = AC */
};

struct fstrength {
  int limit, ilevel, hev;
};

struct dec {
  int w, h, mbw, mbh;
  struct bd br;
  struct bd parts[8];
  int nparts;

  int use_segment, update_map, abs_delta;
  int seg_quant[4], seg_filter[4];
  uint8_t seg_proba[3];

  int simple, level, sharpness, use_lf_delta;
  int ref_lf_delta[4], mode_lf_delta[4];
  int filter_type;
  struct fstrength fs[4][2]; /* [segment][is_i4x4] */

  struct quant q[4];
  uint8_t coef[4][8][3][11];
  int use_skip, skip_proba;

  uint8_t *yp, *up, *vp;
  int ystride, uvstride;

  uint8_t *intra_top;   /* 4 modes per macroblock column */
  struct nzctx *nz_top; /* per macroblock column         */
  uint8_t *mbf;         /* per macroblock: seg | i4x4 << 2 | inner << 3 */
  uint8_t intra_left[4];
  struct nzctx nz_left;

  uint8_t ybuf[17 * BPS], ubuf[9 * BPS], vbuf[9 * BPS];
  int16_t coeffs[384];
};

#define YOFF (BPS + 8)
#define UVOFF (BPS + 8)

static int clip_int(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }
static uint8_t clip8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* ====================================================================== *
 *  Frame header                                                           *
 * ====================================================================== */

static void parse_segment_header(struct dec *d)
{
  struct bd *br = &d->br;
  d->use_segment = bd_bit(br, 0x80);
  d->update_map = 0;
  d->abs_delta = 1;
  memset(d->seg_proba, 255, sizeof d->seg_proba);
  if (!d->use_segment)
    return;
  d->update_map = bd_bit(br, 0x80);
  if (bd_bit(br, 0x80)) {
    d->abs_delta = bd_bit(br, 0x80);
    for (int s = 0; s < 4; s++)
      d->seg_quant[s] = bd_bit(br, 0x80) ? bd_signed_value(br, 7) : 0;
    for (int s = 0; s < 4; s++)
      d->seg_filter[s] = bd_bit(br, 0x80) ? bd_signed_value(br, 6) : 0;
  }
  if (d->update_map)
    for (int s = 0; s < 3; s++)
      d->seg_proba[s] = bd_bit(br, 0x80) ? (uint8_t)bd_value(br, 8) : 255;
}

static void parse_filter_header(struct dec *d)
{
  struct bd *br = &d->br;
  d->simple = bd_bit(br, 0x80);
  d->level = (int)bd_value(br, 6);
  d->sharpness = (int)bd_value(br, 3);
  d->use_lf_delta = bd_bit(br, 0x80);
  if (d->use_lf_delta && bd_bit(br, 0x80)) {
    for (int i = 0; i < 4; i++)
      if (bd_bit(br, 0x80))
        d->ref_lf_delta[i] = bd_signed_value(br, 6);
    for (int i = 0; i < 4; i++)
      if (bd_bit(br, 0x80))
        d->mode_lf_delta[i] = bd_signed_value(br, 6);
  }
  d->filter_type = d->level == 0 ? 0 : d->simple ? 1 : 2;
}

static int parse_partitions(struct dec *d, const uint8_t *buf, int size)
{
  d->nparts = 1 << bd_value(&d->br, 2);
  int last = d->nparts - 1;
  if (size < 3 * last)
    return VP8_EFORMAT;
  const uint8_t *sz = buf;
  const uint8_t *start = buf + last * 3;
  int left = size - last * 3;
  for (int p = 0; p < last; p++) {
    int psize = sz[0] | (sz[1] << 8) | (sz[2] << 16);
    if (psize > left)
      psize = left;
    bd_init(&d->parts[p], start, psize);
    start += psize;
    left -= psize;
    sz += 3;
  }
  bd_init(&d->parts[last], start, left);
  return VP8_OK;
}

static void parse_quant(struct dec *d)
{
  struct bd *br = &d->br;
  int base_q0 = (int)bd_value(br, 7);
  int dqy1_dc = bd_bit(br, 0x80) ? bd_signed_value(br, 4) : 0;
  int dqy2_dc = bd_bit(br, 0x80) ? bd_signed_value(br, 4) : 0;
  int dqy2_ac = bd_bit(br, 0x80) ? bd_signed_value(br, 4) : 0;
  int dquv_dc = bd_bit(br, 0x80) ? bd_signed_value(br, 4) : 0;
  int dquv_ac = bd_bit(br, 0x80) ? bd_signed_value(br, 4) : 0;
  for (int i = 0; i < 4; i++) {
    int q;
    if (d->use_segment) {
      q = d->seg_quant[i];
      if (!d->abs_delta)
        q += base_q0;
    } else {
      if (i > 0) {
        d->q[i] = d->q[0];
        continue;
      }
      q = base_q0;
    }
    struct quant *m = &d->q[i];
    m->y1[0] = kDcTable[clip_int(q + dqy1_dc, 127)];
    m->y1[1] = kAcTable[clip_int(q, 127)];
    m->y2[0] = kDcTable[clip_int(q + dqy2_dc, 127)] * 2;
    m->y2[1] = (kAcTable[clip_int(q + dqy2_ac, 127)] * 101581) >> 16;
    if (m->y2[1] < 8)
      m->y2[1] = 8;
    m->uv[0] = kDcTable[clip_int(q + dquv_dc, 117)];
    m->uv[1] = kAcTable[clip_int(q + dquv_ac, 127)];
  }
}

static void parse_proba(struct dec *d)
{
  struct bd *br = &d->br;
  for (int t = 0; t < 4; t++)
    for (int b = 0; b < 8; b++)
      for (int c = 0; c < 3; c++)
        for (int p = 0; p < 11; p++) {
          int i = ((t * 8 + b) * 3 + c) * 11 + p;
          d->coef[t][b][c][p] = bd_bit(br, kCoeffsUpdate[i]) ? (uint8_t)bd_value(br, 8) : kCoeffsDefault[i];
        }
  d->use_skip = bd_bit(br, 0x80);
  if (d->use_skip)
    d->skip_proba = (int)bd_value(br, 8);
}

static void precompute_filter_strengths(struct dec *d)
{
  for (int s = 0; s < 4; s++) {
    int base;
    if (d->use_segment) {
      base = d->seg_filter[s];
      if (!d->abs_delta)
        base += d->level;
    } else {
      base = d->level;
    }
    for (int i4 = 0; i4 <= 1; i4++) {
      struct fstrength *f = &d->fs[s][i4];
      int level = base;
      if (d->use_lf_delta) {
        level += d->ref_lf_delta[0];
        if (i4)
          level += d->mode_lf_delta[0];
      }
      level = clip_int(level, 63);
      if (level > 0) {
        int ilevel = level;
        if (d->sharpness > 0) {
          ilevel >>= d->sharpness > 4 ? 2 : 1;
          if (ilevel > 9 - d->sharpness)
            ilevel = 9 - d->sharpness;
        }
        if (ilevel < 1)
          ilevel = 1;
        f->ilevel = ilevel;
        f->limit = 2 * level + ilevel;
        f->hev = level >= 40 ? 2 : level >= 15 ? 1 : 0;
      } else {
        f->limit = 0;
      }
    }
  }
}

static int parse_headers(struct dec *d, const uint8_t *data, int size)
{
  if (size < 10)
    return VP8_EFORMAT;
  unsigned bits = data[0] | (data[1] << 8) | (data[2] << 16);
  if (bits & 1)
    return VP8_EFORMAT; /* inter frame: never in a WebP */
  if (((bits >> 1) & 7) > 3 || !((bits >> 4) & 1))
    return VP8_EFORMAT; /* unknown profile / frame not shown */
  unsigned part_len = bits >> 5;
  if (data[3] != 0x9d || data[4] != 0x01 || data[5] != 0x2a)
    return VP8_EFORMAT;
  d->w = (data[6] | (data[7] << 8)) & 0x3fff;
  d->h = (data[8] | (data[9] << 8)) & 0x3fff;
  if (!d->w || !d->h)
    return VP8_EFORMAT;
  data += 10;
  size -= 10;
  if (part_len > (unsigned)size)
    return VP8_EFORMAT;
  bd_init(&d->br, data, (int)part_len);

  bd_bit(&d->br, 0x80); /* colour space */
  bd_bit(&d->br, 0x80); /* clamp type   */
  parse_segment_header(d);
  parse_filter_header(d);
  if (parse_partitions(d, data + part_len, size - (int)part_len) != VP8_OK)
    return VP8_EFORMAT;
  parse_quant(d);
  bd_bit(&d->br, 0x80); /* refresh_entropy_probs, ignored on a key frame */
  parse_proba(d);
  precompute_filter_strengths(d);
  return VP8_OK;
}

/* ====================================================================== *
 *  Per-macroblock syntax                                                  *
 * ====================================================================== */

struct mbinfo {
  int segment, skip, is_i4x4, ymode, uvmode;
  uint8_t imodes[16];
};

static void parse_intra_mode(struct dec *d, int mbx, struct mbinfo *mb)
{
  struct bd *br = &d->br;
  uint8_t *top = d->intra_top + 4 * mbx;
  uint8_t *left = d->intra_left;

  mb->segment = 0;
  if (d->update_map)
    mb->segment = !bd_bit(br, d->seg_proba[0]) ? bd_bit(br, d->seg_proba[1]) : bd_bit(br, d->seg_proba[2]) + 2;
  mb->skip = d->use_skip ? bd_bit(br, d->skip_proba) : 0;
  mb->is_i4x4 = !bd_bit(br, 145);
  if (!mb->is_i4x4) {
    int ymode = bd_bit(br, 156) ? (bd_bit(br, 128) ? B_TM_PRED : B_HE_PRED) : (bd_bit(br, 163) ? B_VE_PRED : B_DC_PRED);
    mb->ymode = ymode;
    memset(top, ymode, 4);
    memset(left, ymode, 4);
  } else {
    uint8_t *modes = mb->imodes;
    for (int y = 0; y < 4; y++) {
      int ymode = left[y];
      for (int x = 0; x < 4; x++) {
        const uint8_t *prob = kBModesProba + (top[x] * 10 + ymode) * 9;
        int i = kYModesIntra4[bd_bit(br, prob[0])];
        while (i > 0)
          i = kYModesIntra4[2 * i + bd_bit(br, prob[i])];
        ymode = -i;
        top[x] = (uint8_t)ymode;
      }
      memcpy(modes, top, 4);
      modes += 4;
      left[y] = (uint8_t)ymode;
    }
  }
  mb->uvmode = !bd_bit(br, 142) ? B_DC_PRED : !bd_bit(br, 114) ? B_VE_PRED : bd_bit(br, 183) ? B_TM_PRED : B_HE_PRED;
}

static int large_value(struct bd *br, const uint8_t *p)
{
  int v;
  if (!bd_bit(br, p[3])) {
    if (!bd_bit(br, p[4]))
      v = 2;
    else
      v = 3 + bd_bit(br, p[5]);
  } else if (!bd_bit(br, p[6])) {
    if (!bd_bit(br, p[7])) {
      v = 5 + bd_bit(br, 159);
    } else {
      v = 7 + 2 * bd_bit(br, 165);
      v += bd_bit(br, 145);
    }
  } else {
    int bit1 = bd_bit(br, p[8]);
    int bit0 = bd_bit(br, p[9 + bit1]);
    int cat = 2 * bit1 + bit0;
    v = 0;
    for (const uint8_t *tab = kCat3456[cat]; *tab; tab++)
      v += v + bd_bit(br, *tab);
    v += 3 + (8 << cat);
  }
  return v;
}

/* Returns the position after the last non-zero coefficient (or 16). */
static int get_coeffs(struct bd *br, const uint8_t (*pr)[3][11], int ctx, const int *dq, int n, int16_t *out)
{
  const uint8_t *p = pr[kBands[n]][ctx];
  for (; n < 16; n++) {
    if (!bd_bit(br, p[0]))
      return n;
    while (!bd_bit(br, p[1])) {
      p = pr[kBands[++n]][0];
      if (n == 16)
        return 16;
    }
    const uint8_t (*pn)[11] = pr[kBands[n + 1]];
    int v;
    if (!bd_bit(br, p[2])) {
      v = 1;
      p = pn[1];
    } else {
      v = large_value(br, p);
      p = pn[2];
    }
    out[kZigzag[n]] = (int16_t)(bd_sign(br, v) * dq[n > 0]);
  }
  return 16;
}

static void transform_wht(const int16_t *in, int16_t *out)
{
  int tmp[16];
  for (int i = 0; i < 4; i++) {
    int a0 = in[0 + i] + in[12 + i];
    int a1 = in[4 + i] + in[8 + i];
    int a2 = in[4 + i] - in[8 + i];
    int a3 = in[0 + i] - in[12 + i];
    tmp[0 + i] = a0 + a1;
    tmp[8 + i] = a0 - a1;
    tmp[4 + i] = a3 + a2;
    tmp[12 + i] = a3 - a2;
  }
  for (int i = 0; i < 4; i++) {
    int dc = tmp[0 + i * 4] + 3;
    int a0 = dc + tmp[3 + i * 4];
    int a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
    int a2 = tmp[1 + i * 4] - tmp[2 + i * 4];
    int a3 = dc - tmp[3 + i * 4];
    out[0] = (int16_t)((a0 + a1) >> 3);
    out[16] = (int16_t)((a3 + a2) >> 3);
    out[32] = (int16_t)((a0 - a1) >> 3);
    out[48] = (int16_t)((a3 - a2) >> 3);
    out += 64;
  }
}

/* Returns 1 if the macroblock carries any non-zero residual. */
static int parse_residuals(struct dec *d, int mbx, const struct mbinfo *mb, struct bd *br)
{
  struct nzctx *top = &d->nz_top[mbx];
  struct nzctx *left = &d->nz_left;
  const struct quant *q = &d->q[mb->segment];
  int16_t *dst = d->coeffs;
  int nonzero = 0, first;
  const uint8_t (*ac)[3][11];

  memset(dst, 0, sizeof d->coeffs);
  if (!mb->is_i4x4) {
    int16_t dc[16] = {0};
    int ctx = top->dc + left->dc;
    int nz = get_coeffs(br, d->coef[1], ctx, q->y2, 0, dc);
    top->dc = left->dc = nz > 0;
    if (nz > 1) {
      transform_wht(dc, dst);
    } else {
      int dc0 = (dc[0] + 3) >> 3;
      for (int i = 0; i < 16 * 16; i += 16)
        dst[i] = (int16_t)dc0;
    }
    first = 1;
    ac = d->coef[0];
  } else {
    first = 0;
    ac = d->coef[3];
  }

  for (int y = 0; y < 4; y++) {
    int l = left->y[y];
    for (int x = 0; x < 4; x++) {
      int nz = get_coeffs(br, ac, l + top->y[x], q->y1, first, dst);
      l = nz > first;
      top->y[x] = (uint8_t)l;
      nonzero |= nz > 1 || dst[0] != 0;
      dst += 16;
    }
    left->y[y] = (uint8_t)l;
  }
  for (int ch = 0; ch < 2; ch++) {
    uint8_t *tn = ch ? top->v : top->u;
    uint8_t *ln = ch ? left->v : left->u;
    for (int y = 0; y < 2; y++) {
      int l = ln[y];
      for (int x = 0; x < 2; x++) {
        int nz = get_coeffs(br, d->coef[2], l + tn[x], q->uv, 0, dst);
        l = nz > 0;
        tn[x] = (uint8_t)l;
        nonzero |= nz > 1 || dst[0] != 0;
        dst += 16;
      }
      ln[y] = (uint8_t)l;
    }
  }
  return nonzero;
}

/* ====================================================================== *
 *  Reconstruction                                                         *
 * ====================================================================== */

/* Unsigned multiply: hostile coefficients may overflow, which must wrap rather than be UB. */
#define MUL1(a) (((int)((unsigned)(a) * 20091u) >> 16) + (a))
#define MUL2(a) ((int)((unsigned)(a) * 35468u) >> 16)

static void transform_add(const int16_t *in, uint8_t *dst)
{
  int tmp[16], *t = tmp;
  for (int i = 0; i < 4; i++) {
    int a = in[0] + in[8];
    int b = in[0] - in[8];
    int c = MUL2(in[4]) - MUL1(in[12]);
    int e = MUL1(in[4]) + MUL2(in[12]);
    t[0] = a + e;
    t[1] = b + c;
    t[2] = b - c;
    t[3] = a - e;
    t += 4;
    in++;
  }
  t = tmp;
  for (int i = 0; i < 4; i++) {
    int dc = t[0] + 4;
    int a = dc + t[8];
    int b = dc - t[8];
    int c = MUL2(t[4]) - MUL1(t[12]);
    int e = MUL1(t[4]) + MUL2(t[12]);
    dst[0] = clip8(dst[0] + ((a + e) >> 3));
    dst[1] = clip8(dst[1] + ((b + c) >> 3));
    dst[2] = clip8(dst[2] + ((b - c) >> 3));
    dst[3] = clip8(dst[3] + ((a - e) >> 3));
    t++;
    dst += BPS;
  }
}

static void add_residual(const int16_t *in, uint8_t *dst)
{
  for (int i = 0; i < 16; i++)
    if (in[i]) {
      transform_add(in, dst);
      return;
    }
}

#define AVG3(a, b, c) (((a) + 2 * (b) + (c) + 2) >> 2)
#define AVG2(a, b) (((a) + (b) + 1) >> 1)

/* 4x4 intra prediction from the left (4), top-left, top (4) and top-right (4) pixels. */
static void predict4(uint8_t *dst, int mode, const uint8_t *left, int tl, const uint8_t *top)
{
#define DST(x, y) dst[(x) + (y) * BPS]
  int I = left[0], J = left[1], K = left[2], L = left[3], X = tl;
  int A = top[0], B = top[1], C = top[2], D = top[3], E = top[4], F = top[5], G = top[6], H = top[7];
  switch (mode) {
  case B_DC_PRED: {
    int dc = 4 + A + B + C + D + I + J + K + L;
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++)
        DST(x, y) = (uint8_t)(dc >> 3);
    break;
  }
  case B_TM_PRED:
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++)
        DST(x, y) = clip8(top[x] + left[y] - tl);
    break;
  case B_VE_PRED: {
    uint8_t v[4] = {(uint8_t)AVG3(X, A, B), (uint8_t)AVG3(A, B, C), (uint8_t)AVG3(B, C, D), (uint8_t)AVG3(C, D, E)};
    for (int y = 0; y < 4; y++)
      memcpy(&DST(0, y), v, 4);
    break;
  }
  case B_HE_PRED: {
    uint8_t v[4] = {(uint8_t)AVG3(X, I, J), (uint8_t)AVG3(I, J, K), (uint8_t)AVG3(J, K, L), (uint8_t)AVG3(K, L, L)};
    for (int y = 0; y < 4; y++)
      memset(&DST(0, y), v[y], 4);
    break;
  }
  case B_RD_PRED:
    DST(0, 3) = AVG3(J, K, L);
    DST(1, 3) = DST(0, 2) = AVG3(I, J, K);
    DST(2, 3) = DST(1, 2) = DST(0, 1) = AVG3(X, I, J);
    DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = AVG3(A, X, I);
    DST(3, 2) = DST(2, 1) = DST(1, 0) = AVG3(B, A, X);
    DST(3, 1) = DST(2, 0) = AVG3(C, B, A);
    DST(3, 0) = AVG3(D, C, B);
    break;
  case B_VR_PRED:
    DST(0, 0) = DST(1, 2) = AVG2(X, A);
    DST(1, 0) = DST(2, 2) = AVG2(A, B);
    DST(2, 0) = DST(3, 2) = AVG2(B, C);
    DST(3, 0) = AVG2(C, D);
    DST(0, 3) = AVG3(K, J, I);
    DST(0, 2) = AVG3(J, I, X);
    DST(0, 1) = DST(1, 3) = AVG3(I, X, A);
    DST(1, 1) = DST(2, 3) = AVG3(X, A, B);
    DST(2, 1) = DST(3, 3) = AVG3(A, B, C);
    DST(3, 1) = AVG3(B, C, D);
    break;
  case B_LD_PRED:
    DST(0, 0) = AVG3(A, B, C);
    DST(1, 0) = DST(0, 1) = AVG3(B, C, D);
    DST(2, 0) = DST(1, 1) = DST(0, 2) = AVG3(C, D, E);
    DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = AVG3(D, E, F);
    DST(3, 1) = DST(2, 2) = DST(1, 3) = AVG3(E, F, G);
    DST(3, 2) = DST(2, 3) = AVG3(F, G, H);
    DST(3, 3) = AVG3(G, H, H);
    break;
  case B_VL_PRED:
    DST(0, 0) = AVG2(A, B);
    DST(1, 0) = DST(0, 2) = AVG2(B, C);
    DST(2, 0) = DST(1, 2) = AVG2(C, D);
    DST(3, 0) = DST(2, 2) = AVG2(D, E);
    DST(0, 1) = AVG3(A, B, C);
    DST(1, 1) = DST(0, 3) = AVG3(B, C, D);
    DST(2, 1) = DST(1, 3) = AVG3(C, D, E);
    DST(3, 1) = DST(2, 3) = AVG3(D, E, F);
    DST(3, 2) = AVG3(E, F, G);
    DST(3, 3) = AVG3(F, G, H);
    break;
  case B_HD_PRED:
    DST(0, 0) = DST(2, 1) = AVG2(I, X);
    DST(0, 1) = DST(2, 2) = AVG2(J, I);
    DST(0, 2) = DST(2, 3) = AVG2(K, J);
    DST(0, 3) = AVG2(L, K);
    DST(3, 0) = AVG3(A, B, C);
    DST(2, 0) = AVG3(X, A, B);
    DST(1, 0) = DST(3, 1) = AVG3(I, X, A);
    DST(1, 1) = DST(3, 2) = AVG3(J, I, X);
    DST(1, 2) = DST(3, 3) = AVG3(K, J, I);
    DST(1, 3) = AVG3(L, K, J);
    break;
  default: /* B_HU_PRED */
    DST(0, 0) = AVG2(I, J);
    DST(2, 0) = DST(0, 1) = AVG2(J, K);
    DST(2, 1) = DST(0, 2) = AVG2(K, L);
    DST(1, 0) = AVG3(I, J, K);
    DST(3, 0) = DST(1, 1) = AVG3(J, K, L);
    DST(3, 1) = DST(1, 2) = AVG3(K, L, L);
    DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = L;
    break;
  }
#undef DST
}

/* 16x16 luma / 8x8 chroma prediction; the borders already hold 127 / 129 where the frame ends. */
static void predict_block(uint8_t *dst, int size, int mode, int has_top, int has_left)
{
  const uint8_t *top = dst - BPS;
  int tl = top[-1];
  switch (mode) {
  case B_DC_PRED: {
    int sum = 0, dc, log2 = size == 16 ? 4 : 3;
    if (has_top)
      for (int i = 0; i < size; i++)
        sum += top[i];
    if (has_left)
      for (int i = 0; i < size; i++)
        sum += dst[i * BPS - 1];
    if (has_top && has_left)
      dc = (sum + size) >> (log2 + 1);
    else if (has_top || has_left)
      dc = (sum + size / 2) >> log2;
    else
      dc = 128;
    for (int y = 0; y < size; y++)
      memset(dst + y * BPS, dc, (unsigned)size);
    break;
  }
  case B_TM_PRED:
    for (int y = 0; y < size; y++)
      for (int x = 0; x < size; x++)
        dst[y * BPS + x] = clip8(top[x] + dst[y * BPS - 1] - tl);
    break;
  case B_VE_PRED:
    for (int y = 0; y < size; y++)
      memcpy(dst + y * BPS, top, (unsigned)size);
    break;
  default: /* B_HE_PRED */
    for (int y = 0; y < size; y++)
      memset(dst + y * BPS, dst[y * BPS - 1], (unsigned)size);
    break;
  }
}

/* Fill the work buffer borders (row above, column to the left) for one plane of a macroblock. */
static void load_borders(uint8_t *buf, const uint8_t *plane, int stride, int size, int mbx, int mby, int mbw, int tr)
{
  uint8_t *b = buf - BPS - 1; /* top-left corner */
  const uint8_t *src = plane + mby * size * stride + mbx * size;
  if (mby == 0) {
    memset(b, 127, (unsigned)(1 + size + (tr ? 4 : 0)));
  } else {
    const uint8_t *row = src - stride;
    b[0] = mbx == 0 ? 129 : row[-1];
    memcpy(b + 1, row, (unsigned)size);
    if (tr) {
      if (mbx < mbw - 1)
        memcpy(b + 1 + size, row + size, 4);
      else
        memset(b + 1 + size, row[size - 1], 4);
    }
  }
  for (int j = 0; j < size; j++)
    buf[j * BPS - 1] = mbx == 0 ? 129 : src[j * stride - 1];
}

static void store_block(uint8_t *plane, int stride, const uint8_t *buf, int size, int mbx, int mby)
{
  uint8_t *dst = plane + mby * size * stride + mbx * size;
  for (int j = 0; j < size; j++)
    memcpy(dst + j * stride, buf + j * BPS, (unsigned)size);
}

static void reconstruct_mb(struct dec *d, int mbx, int mby, const struct mbinfo *mb, int has_coeffs)
{
  uint8_t *y = d->ybuf + YOFF, *u = d->ubuf + UVOFF, *v = d->vbuf + UVOFF;
  const int16_t *co = d->coeffs;

  load_borders(y, d->yp, d->ystride, 16, mbx, mby, d->mbw, 1);
  load_borders(u, d->up, d->uvstride, 8, mbx, mby, d->mbw, 0);
  load_borders(v, d->vp, d->uvstride, 8, mbx, mby, d->mbw, 0);

  if (!mb->is_i4x4) {
    predict_block(y, 16, mb->ymode, mby > 0, mbx > 0);
    if (has_coeffs)
      for (int n = 0; n < 16; n++)
        add_residual(co + n * 16, y + (n & 3) * 4 + (n >> 2) * 4 * BPS);
  } else {
    uint8_t tr[4];
    memcpy(tr, y - BPS + 16, 4);
    for (int n = 0; n < 16; n++) {
      int bx = n & 3, by = n >> 2;
      uint8_t *blk = y + bx * 4 + by * 4 * BPS;
      uint8_t top[8], left[4];
      memcpy(top, blk - BPS, 4);
      if (bx < 3)
        memcpy(top + 4, blk - BPS + 4, 4);
      else
        memcpy(top + 4, tr, 4);
      for (int j = 0; j < 4; j++)
        left[j] = blk[j * BPS - 1];
      predict4(blk, mb->imodes[n], left, blk[-BPS - 1], top);
      if (has_coeffs)
        add_residual(co + n * 16, blk);
    }
  }

  predict_block(u, 8, mb->uvmode, mby > 0, mbx > 0);
  predict_block(v, 8, mb->uvmode, mby > 0, mbx > 0);
  if (has_coeffs)
    for (int n = 0; n < 4; n++) {
      int off = (n & 1) * 4 + (n >> 1) * 4 * BPS;
      add_residual(co + 256 + n * 16, u + off);
      add_residual(co + 320 + n * 16, v + off);
    }

  store_block(d->yp, d->ystride, y, 16, mbx, mby);
  store_block(d->up, d->uvstride, u, 8, mbx, mby);
  store_block(d->vp, d->uvstride, v, 8, mbx, mby);
}

/* ====================================================================== *
 *  In-loop deblocking filter (RFC 6386 section 15)                        *
 * ====================================================================== */

static int sclip1(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static int sclip2(int v) { return v < -16 ? -16 : v > 15 ? 15 : v; }
static int iabs(int v) { return v < 0 ? -v : v; }

static void filter2(uint8_t *p, int step)
{
  int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
  int a = 3 * (q0 - p0) + sclip1(p1 - q1);
  int a1 = sclip2((a + 4) >> 3);
  int a2 = sclip2((a + 3) >> 3);
  p[-step] = clip8(p0 + a2);
  p[0] = clip8(q0 - a1);
}

static void filter4(uint8_t *p, int step)
{
  int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
  int a = 3 * (q0 - p0);
  int a1 = sclip2((a + 4) >> 3);
  int a2 = sclip2((a + 3) >> 3);
  int a3 = (a1 + 1) >> 1;
  p[-2 * step] = clip8(p1 + a3);
  p[-step] = clip8(p0 + a2);
  p[0] = clip8(q0 - a1);
  p[step] = clip8(q1 - a3);
}

static void filter6(uint8_t *p, int step)
{
  int p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
  int q0 = p[0], q1 = p[step], q2 = p[2 * step];
  int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
  int a1 = (27 * a + 63) >> 7;
  int a2 = (18 * a + 63) >> 7;
  int a3 = (9 * a + 63) >> 7;
  p[-3 * step] = clip8(p2 + a3);
  p[-2 * step] = clip8(p1 + a2);
  p[-step] = clip8(p0 + a1);
  p[0] = clip8(q0 - a1);
  p[step] = clip8(q1 - a2);
  p[2 * step] = clip8(q2 - a3);
}

static int hev(const uint8_t *p, int step, int thresh)
{
  return iabs(p[-2 * step] - p[-step]) > thresh || iabs(p[step] - p[0]) > thresh;
}

static int needs_filter(const uint8_t *p, int step, int t)
{
  return 4 * iabs(p[-step] - p[0]) + iabs(p[-2 * step] - p[step]) <= t;
}

static int needs_filter2(const uint8_t *p, int step, int t, int it)
{
  if (4 * iabs(p[-step] - p[0]) + iabs(p[-2 * step] - p[step]) > t)
    return 0;
  return iabs(p[-4 * step] - p[-3 * step]) <= it && iabs(p[-3 * step] - p[-2 * step]) <= it &&
         iabs(p[-2 * step] - p[-step]) <= it && iabs(p[3 * step] - p[2 * step]) <= it &&
         iabs(p[2 * step] - p[step]) <= it && iabs(p[step] - p[0]) <= it;
}

/* Filter `size` pixels along an edge: `hs` steps across it, `vs` steps along it. */
static void simple_edge(uint8_t *p, int hs, int vs, int size, int thresh)
{
  int t2 = 2 * thresh + 1;
  for (int i = 0; i < size; i++, p += vs)
    if (needs_filter(p, hs, t2))
      filter2(p, hs);
}

static void complex_edge(uint8_t *p, int hs, int vs, int size, int thresh, int ithresh, int hev_thresh, int mb_edge)
{
  int t2 = 2 * thresh + 1;
  for (int i = 0; i < size; i++, p += vs) {
    if (!needs_filter2(p, hs, t2, ithresh))
      continue;
    if (hev(p, hs, hev_thresh))
      filter2(p, hs);
    else if (mb_edge)
      filter6(p, hs);
    else
      filter4(p, hs);
  }
}

static void filter_mb(struct dec *d, int mbx, int mby)
{
  int f = d->mbf[mby * d->mbw + mbx];
  const struct fstrength *fs = &d->fs[f & 3][(f >> 2) & 1];
  int inner = (f >> 3) & 1;
  int limit = fs->limit;
  if (limit == 0)
    return;
  int ys = d->ystride, uvs = d->uvstride;
  uint8_t *y = d->yp + mby * 16 * ys + mbx * 16;
  uint8_t *u = d->up + mby * 8 * uvs + mbx * 8;
  uint8_t *v = d->vp + mby * 8 * uvs + mbx * 8;

  if (d->filter_type == 1) {
    if (mbx > 0)
      simple_edge(y, 1, ys, 16, limit + 4);
    if (inner)
      for (int k = 4; k < 16; k += 4)
        simple_edge(y + k, 1, ys, 16, limit);
    if (mby > 0)
      simple_edge(y, ys, 1, 16, limit + 4);
    if (inner)
      for (int k = 4; k < 16; k += 4)
        simple_edge(y + k * ys, ys, 1, 16, limit);
    return;
  }

  int il = fs->ilevel, ht = fs->hev;
  if (mbx > 0) {
    complex_edge(y, 1, ys, 16, limit + 4, il, ht, 1);
    complex_edge(u, 1, uvs, 8, limit + 4, il, ht, 1);
    complex_edge(v, 1, uvs, 8, limit + 4, il, ht, 1);
  }
  if (inner) {
    for (int k = 4; k < 16; k += 4)
      complex_edge(y + k, 1, ys, 16, limit, il, ht, 0);
    complex_edge(u + 4, 1, uvs, 8, limit, il, ht, 0);
    complex_edge(v + 4, 1, uvs, 8, limit, il, ht, 0);
  }
  if (mby > 0) {
    complex_edge(y, ys, 1, 16, limit + 4, il, ht, 1);
    complex_edge(u, uvs, 1, 8, limit + 4, il, ht, 1);
    complex_edge(v, uvs, 1, 8, limit + 4, il, ht, 1);
  }
  if (inner) {
    for (int k = 4; k < 16; k += 4)
      complex_edge(y + k * ys, ys, 1, 16, limit, il, ht, 0);
    complex_edge(u + 4 * uvs, uvs, 1, 8, limit, il, ht, 0);
    complex_edge(v + 4 * uvs, uvs, 1, 8, limit, il, ht, 0);
  }
}

/* ====================================================================== *
 *  YUV 4:2:0 -> RGBA                                                      *
 * ====================================================================== */

static int mult_hi(int v, int coeff) { return (v * coeff) >> 8; }

static uint8_t clip_fix(int v) { return (uint8_t)(((v & ~((256 << 6) - 1)) == 0) ? (v >> 6) : v < 0 ? 0 : 255); }

/* Chroma sample for luma column px, bilinear between the nearer (n) and farther (f) chroma rows. */
static int upsample_px(const uint8_t *n, const uint8_t *f, int cw, int px)
{
  int x = (px + 1) >> 1;
  if (px == 0)
    return (3 * n[0] + f[0] + 2) >> 2;
  if (x >= cw)
    return (3 * n[cw - 1] + f[cw - 1] + 2) >> 2;
  int n0 = n[x - 1], n1 = n[x], f0 = f[x - 1], f1 = f[x];
  int s = n0 + n1 + f0 + f1 + 8;
  if (px & 1)
    return (((s + 2 * (n1 + f0)) >> 3) + n0) >> 1;
  return (((s + 2 * (n0 + f1)) >> 3) + n1) >> 1;
}

static void emit_rgba(const struct dec *d, uint8_t *out)
{
  int cw = (d->w + 1) >> 1, ch = (d->h + 1) >> 1;
  for (int y = 0; y < d->h; y++) {
    int near_row = y >> 1;
    int far_row = (y & 1) ? near_row + 1 : near_row - 1;
    if (far_row < 0 || far_row >= ch)
      far_row = near_row;
    const uint8_t *yrow = d->yp + y * d->ystride;
    const uint8_t *nu = d->up + near_row * d->uvstride, *nv = d->vp + near_row * d->uvstride;
    const uint8_t *fu = d->up + far_row * d->uvstride, *fv = d->vp + far_row * d->uvstride;
    uint8_t *dst = out + (unsigned)y * d->w * 4;
    for (int x = 0; x < d->w; x++) {
      int u = upsample_px(nu, fu, cw, x), v = upsample_px(nv, fv, cw, x);
      int yy = mult_hi(yrow[x], 19077);
      dst[0] = clip_fix(yy + mult_hi(v, 26149) - 14234);
      dst[1] = clip_fix(yy - mult_hi(u, 6419) - mult_hi(v, 13320) + 8708);
      dst[2] = clip_fix(yy + mult_hi(u, 33050) - 17685);
      dst[3] = 255;
      dst += 4;
    }
  }
}

/* ====================================================================== *
 *  Entry point                                                            *
 * ====================================================================== */

static void dec_free(struct dec *d)
{
  free(d->yp);
  free(d->up);
  free(d->vp);
  free(d->intra_top);
  free(d->nz_top);
  free(d->mbf);
  free(d);
}

int vp8_decode(const unsigned char *data, int size, unsigned char **out, unsigned *w, unsigned *h)
{
  *out = 0;
  struct dec *d = calloc(1, sizeof *d);
  if (!d)
    return VP8_ENOMEM;
  int rc = parse_headers(d, data, size);
  if (rc != VP8_OK) {
    free(d);
    return rc;
  }

  d->mbw = (d->w + 15) >> 4;
  d->mbh = (d->h + 15) >> 4;
  d->ystride = d->mbw * 16;
  d->uvstride = d->mbw * 8;
  d->yp = malloc((unsigned)d->ystride * d->mbh * 16);
  d->up = malloc((unsigned)d->uvstride * d->mbh * 8);
  d->vp = malloc((unsigned)d->uvstride * d->mbh * 8);
  d->intra_top = calloc((unsigned)d->mbw, 4);
  d->nz_top = calloc((unsigned)d->mbw, sizeof *d->nz_top);
  d->mbf = calloc((unsigned)d->mbw * d->mbh, 1);
  unsigned char *rgba = malloc((unsigned)d->w * d->h * 4);
  if (!d->yp || !d->up || !d->vp || !d->intra_top || !d->nz_top || !d->mbf || !rgba) {
    free(rgba);
    dec_free(d);
    return VP8_ENOMEM;
  }

  for (int mby = 0; mby < d->mbh; mby++) {
    struct bd *tokens = &d->parts[mby & (d->nparts - 1)];
    memset(&d->nz_left, 0, sizeof d->nz_left);
    memset(d->intra_left, B_DC_PRED, sizeof d->intra_left);
    for (int mbx = 0; mbx < d->mbw; mbx++) {
      struct mbinfo mb;
      parse_intra_mode(d, mbx, &mb);
      int has_coeffs = 0;
      if (!mb.skip) {
        has_coeffs = parse_residuals(d, mbx, &mb, tokens);
      } else {
        struct nzctx *top = &d->nz_top[mbx];
        uint8_t dc_t = top->dc, dc_l = d->nz_left.dc;
        memset(top, 0, sizeof *top);
        memset(&d->nz_left, 0, sizeof d->nz_left);
        if (mb.is_i4x4)
          top->dc = dc_t, d->nz_left.dc = dc_l;
      }
      reconstruct_mb(d, mbx, mby, &mb, has_coeffs);
      d->mbf[mby * d->mbw + mbx] = (uint8_t)(mb.segment | mb.is_i4x4 << 2 | (mb.is_i4x4 || has_coeffs) << 3);
    }
  }

  if (d->filter_type > 0)
    for (int mby = 0; mby < d->mbh; mby++)
      for (int mbx = 0; mbx < d->mbw; mbx++)
        filter_mb(d, mbx, mby);

  emit_rgba(d, rgba);
  *out = rgba;
  *w = (unsigned)d->w;
  *h = (unsigned)d->h;
  dec_free(d);
  return VP8_OK;
}
