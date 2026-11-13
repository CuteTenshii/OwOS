/* magic.c: content identification for file(1) (see magic.h). Freestanding: byte reads only. */
#include "magic.h"

typedef unsigned char u8;

struct ctx {
  const u8 *b;
  int len;
  int complete;
  struct magic_info *mi;
  int n; /* bytes written to mi->desc */
};

static void put_ch(struct ctx *c, char ch)
{
  if (c->n + 1 < (int)sizeof c->mi->desc) {
    c->mi->desc[c->n++] = ch;
    c->mi->desc[c->n] = 0;
  }
}

static void put(struct ctx *c, const char *s)
{
  while (*s)
    put_ch(c, *s++);
}

static void put_u64(struct ctx *c, unsigned long long v)
{
  char tmp[20];
  int i = 0;
  do {
    tmp[i++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (i)
    put_ch(c, tmp[--i]);
}

static void put_u(struct ctx *c, unsigned v) { put_u64(c, v); }

static void put_hex(struct ctx *c, unsigned v)
{
  char tmp[8];
  int i = 0;
  put(c, "0x");
  do {
    tmp[i++] = "0123456789abcdef"[v & 15];
    v >>= 4;
  } while (v);
  while (i)
    put_ch(c, tmp[--i]);
}

static void put_dims(struct ctx *c, unsigned w, unsigned h)
{
  put(c, ", ");
  put_u(c, w);
  put(c, " x ");
  put_u(c, h);
}

static void put_channels(struct ctx *c, unsigned n)
{
  if (n == 1) {
    put(c, "mono");
  } else if (n == 2) {
    put(c, "stereo");
  } else {
    put_u(c, n);
    put(c, " channels");
  }
}

static int printable(u8 ch) { return ch >= 0x20 && ch < 0x7f; }

/* A fixed-size, space- or NUL-padded name field as `pre"name"`; skipped when blank or not ASCII. */
static void put_label(struct ctx *c, const char *pre, int off, int n)
{
  if (off < 0 || off + n > c->len)
    return;
  const u8 *p = c->b + off;
  while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == 0))
    n--;
  if (!n)
    return;
  for (int i = 0; i < n; i++)
    if (!printable(p[i]))
      return;
  put(c, pre);
  put_ch(c, '"');
  for (int i = 0; i < n; i++)
    put_ch(c, (char)p[i]);
  put_ch(c, '"');
}

static int has(const struct ctx *c, int off, const char *s, int n)
{
  if (off < 0 || off + n > c->len)
    return 0;
  for (int i = 0; i < n; i++)
    if (c->b[off + i] != (u8)s[i])
      return 0;
  return 1;
}

/* sizeof keeps embedded NULs in the literal, which most magic numbers have. */
#define HAS(c, off, lit) has((c), (off), (lit), (int)sizeof(lit) - 1)

static int slen(const char *s)
{
  int n = 0;
  while (s[n])
    n++;
  return n;
}

static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

static int has_ci(const struct ctx *c, int off, const char *s)
{
  int n = slen(s);
  if (off < 0 || off + n > c->len)
    return 0;
  for (int i = 0; i < n; i++)
    if (lower(c->b[off + i]) != lower((u8)s[i]))
      return 0;
  return 1;
}

static int contains(const struct ctx *c, int from, const char *s)
{
  int n = slen(s);
  for (int i = from; i + n <= c->len; i++)
    if (has(c, i, s, n))
      return 1;
  return 0;
}

/* Bounds-checked integer reads: 0 past the end of the data. */
static unsigned le16(const struct ctx *c, int off)
{
  return off >= 0 && off + 2 <= c->len ? c->b[off] | (unsigned)c->b[off + 1] << 8 : 0;
}

static unsigned le24(const struct ctx *c, int off)
{
  return off >= 0 && off + 3 <= c->len ? le16(c, off) | (unsigned)c->b[off + 2] << 16 : 0;
}

static unsigned le32(const struct ctx *c, int off)
{
  return off >= 0 && off + 4 <= c->len ? le16(c, off) | le16(c, off + 2) << 16 : 0;
}

static unsigned be16(const struct ctx *c, int off)
{
  return off >= 0 && off + 2 <= c->len ? (unsigned)c->b[off] << 8 | c->b[off + 1] : 0;
}

static unsigned be32(const struct ctx *c, int off)
{
  return off >= 0 && off + 4 <= c->len ? be16(c, off) << 16 | be16(c, off + 2) : 0;
}

static unsigned long long be64(const struct ctx *c, int off)
{
  return (unsigned long long)be32(c, off) << 32 | be32(c, off + 4);
}

static unsigned rd16(const struct ctx *c, int off, int msb) { return msb ? be16(c, off) : le16(c, off); }

static unsigned rd32(const struct ctx *c, int off, int msb) { return msb ? be32(c, off) : le32(c, off); }

static void mime(struct ctx *c, const char *m) { c->mi->mime = m; }

static const char *elf_machine(unsigned m)
{
  switch (m) {
  case 3:
    return "Intel 80386";
  case 8:
    return "MIPS";
  case 20:
    return "PowerPC";
  case 21:
    return "64-bit PowerPC";
  case 40:
    return "ARM";
  case 62:
    return "x86-64";
  case 183:
    return "ARM aarch64";
  case 243:
    return "RISC-V";
  default:
    break;
  }
  return 0;
}

static const char *elf_osabi(unsigned abi)
{
  switch (abi) {
  case 0:
    return "SYSV";
  case 3:
    return "GNU/Linux";
  case 6:
    return "Solaris";
  case 9:
    return "FreeBSD";
  case 12:
    return "OpenBSD";
  case 97:
    return "ARM";
  case 255:
    return "embedded";
  default:
    break;
  }
  return 0;
}

/* 1 if a PT_INTERP program header names a dynamic loader, 0 if none, -1 if the headers lie past the data. */
static int elf_interp(const struct ctx *c, int cls, int msb)
{
  unsigned long long off;
  unsigned ent, num;
  if (cls == 1 && c->len >= 52) {
    off = rd32(c, 28, msb);
    ent = rd16(c, 42, msb);
    num = rd16(c, 44, msb);
  } else if (cls == 2 && c->len >= 64) {
    off = msb ? be64(c, 32) : (unsigned long long)le32(c, 36) << 32 | le32(c, 32);
    ent = rd16(c, 54, msb);
    num = rd16(c, 56, msb);
  } else {
    return -1;
  }
  if (ent < 4 || off + (unsigned long long)ent * num > (unsigned)c->len)
    return -1;
  for (unsigned i = 0; i < num; i++)
    if (rd32(c, (int)(off + (unsigned long long)i * ent), msb) == 3)
      return 1;
  return 0;
}

static int elf(struct ctx *c)
{
  if (!HAS(c, 0, "\177ELF") || c->len < 20)
    return 0;
  int cls = c->b[4], msb = c->b[5] == 2;
  unsigned type = rd16(c, 16, msb), mach = rd16(c, 18, msb);
  int interp = elf_interp(c, cls, msb);

  put(c, "ELF ");
  put(c, cls == 2 ? "64-bit " : cls == 1 ? "32-bit " : "invalid class ");
  put(c, msb ? "MSB " : c->b[5] == 1 ? "LSB " : "invalid byte order ");
  switch (type) {
  case 1:
    put(c, "relocatable");
    mime(c, "application/x-object");
    break;
  case 2:
    put(c, "executable");
    mime(c, "application/x-executable");
    break;
  case 3:
    put(c, interp > 0 ? "pie executable" : "shared object");
    mime(c, interp > 0 ? "application/x-pie-executable" : "application/x-sharedlib");
    break;
  case 4:
    put(c, "core file");
    mime(c, "application/x-coredump");
    break;
  default:
    put(c, "unknown type ");
    put_hex(c, type);
    mime(c, "application/octet-stream");
  }

  const char *m = elf_machine(mach);
  put(c, ", ");
  if (m) {
    put(c, m);
  } else {
    put(c, "unknown arch ");
    put_hex(c, mach);
  }
  put(c, ", version ");
  put_u(c, c->b[6]);
  const char *abi = elf_osabi(c->b[7]);
  if (abi) {
    put(c, " (");
    put(c, abi);
    put(c, ")");
  }
  if (type == 2 && interp >= 0)
    put(c, interp ? ", dynamically linked" : ", statically linked");
  else if (type == 3 && interp > 0)
    put(c, ", dynamically linked");
  return 1;
}

static const char *pe_machine(unsigned m)
{
  switch (m) {
  case 0x14c:
    return "Intel 80386";
  case 0x8664:
    return "x86-64";
  case 0xaa64:
    return "Aarch64";
  case 0x1c0:
    return "ARM";
  case 0x1c4:
    return "ARMv7 Thumb";
  case 0x5064:
    return "RISC-V 64-bit";
  default:
    break;
  }
  return 0;
}

static const char *pe_subsystem(unsigned s)
{
  switch (s) {
  case 1:
    return "native";
  case 2:
    return "GUI";
  case 3:
    return "console";
  case 10:
    return "EFI application";
  case 11:
    return "EFI boot service driver";
  case 12:
    return "EFI runtime driver";
  default:
    break;
  }
  return 0;
}

static int pe(struct ctx *c)
{
  if (!HAS(c, 0, "MZ") || c->len < 64)
    return 0;
  unsigned hdr = le32(c, 0x3c);
  /* The optional header's subsystem field sits 68 bytes in for both PE32 and PE32+. */
  if (hdr >= 64 && hdr < (unsigned)c->len && hdr + 24 + 70 <= (unsigned)c->len && HAS(c, (int)hdr, "PE\0\0")) {
    int h = (int)hdr;
    const char *sub = pe_subsystem(le16(c, h + 24 + 68)), *m = pe_machine(le16(c, h + 4));
    put(c, le16(c, h + 24) == 0x20b ? "PE32+ executable" : "PE32 executable");
    if (le16(c, h + 22) & 0x2000)
      put(c, " (DLL)");
    if (sub) {
      put(c, " (");
      put(c, sub);
      put(c, ")");
    }
    put(c, " ");
    if (m) {
      put(c, m);
    } else {
      put(c, "unknown arch ");
      put_hex(c, le16(c, h + 4));
    }
    mime(c, "application/vnd.microsoft.portable-executable");
    return 1;
  }
  put(c, "MS-DOS executable");
  mime(c, "application/x-dosexec");
  return 1;
}

static int macho(struct ctx *c)
{
  if (c->len < 16)
    return 0;
  int msb, bits;
  switch (be32(c, 0)) {
  case 0xfeedface:
    msb = 1, bits = 32;
    break;
  case 0xcefaedfe:
    msb = 0, bits = 32;
    break;
  case 0xfeedfacf:
    msb = 1, bits = 64;
    break;
  case 0xcffaedfe:
    msb = 0, bits = 64;
    break;
  default:
    return 0;
  }
  unsigned cpu = rd32(c, 4, msb), type = rd32(c, 12, msb);
  put(c, bits == 64 ? "Mach-O 64-bit " : "Mach-O ");
  switch (cpu) {
  case 7:
    put(c, "i386");
    break;
  case 0x01000007:
    put(c, "x86_64");
    break;
  case 12:
    put(c, "arm");
    break;
  case 0x0100000c:
    put(c, "arm64");
    break;
  case 18:
    put(c, "ppc");
    break;
  default:
    put(c, "cpu ");
    put_hex(c, cpu);
  }
  switch (type) {
  case 1:
    put(c, " object");
    break;
  case 2:
    put(c, " executable");
    break;
  case 4:
    put(c, " core");
    break;
  case 6:
    put(c, " dynamically linked shared library");
    break;
  case 8:
    put(c, " bundle");
    break;
  default:
    put(c, " file type ");
    put_u(c, type);
  }
  mime(c, "application/x-mach-binary");
  return 1;
}

/* 0xCAFEBABE opens both Java classes and Mach-O fat binaries; a fat header's architecture count is
 * small, where a class file's major version is 45 or more. */
static int cafebabe(struct ctx *c)
{
  if (!HAS(c, 0, "\xca\xfe\xba\xbe") || c->len < 8)
    return 0;
  unsigned n = be32(c, 4);
  if (n >= 1 && n <= 20) {
    put(c, "Mach-O universal binary with ");
    put_u(c, n);
    put(c, n == 1 ? " architecture" : " architectures");
    mime(c, "application/x-mach-binary");
    return 1;
  }
  if (be16(c, 6) < 45)
    return 0;
  put(c, "compiled Java class data, version ");
  put_u(c, be16(c, 6));
  put(c, ".");
  put_u(c, be16(c, 4));
  mime(c, "application/x-java-applet");
  return 1;
}

static int wasm(struct ctx *c)
{
  if (!HAS(c, 0, "\0asm") || c->len < 8)
    return 0;
  put(c, "WebAssembly (wasm) binary module version ");
  put_hex(c, le32(c, 4));
  if (le32(c, 4) == 1)
    put(c, " (MVP)");
  mime(c, "application/wasm");
  return 1;
}

static int png(struct ctx *c)
{
  if (!HAS(c, 0, "\x89PNG\r\n\x1a\n"))
    return 0;
  put(c, "PNG image data");
  mime(c, "image/png");
  if (!HAS(c, 12, "IHDR") || c->len < 29)
    return 1;
  static const char *const kinds[] = {"-bit grayscale",  0, "-bit/color RGB", "-bit colormap",
                                      "-bit gray+alpha", 0, "-bit/color RGBA"};
  unsigned type = c->b[25];
  put_dims(c, be32(c, 16), be32(c, 20));
  put(c, ", ");
  put_u(c, c->b[24]);
  put(c, type < 7 && kinds[type] ? kinds[type] : "-bit");
  put(c, c->b[28] ? ", interlaced" : ", non-interlaced");
  return 1;
}

static int jpeg(struct ctx *c)
{
  if (!HAS(c, 0, "\xff\xd8\xff"))
    return 0;
  put(c, "JPEG image data");
  mime(c, "image/jpeg");
  int pos = 2;
  while (pos + 4 <= c->len && c->b[pos] == 0xff) {
    unsigned m = c->b[pos + 1];
    if (m == 0xff) {
      pos++;
      continue;
    }
    if (m == 0x01 || (m >= 0xd0 && m <= 0xd8)) {
      pos += 2;
      continue;
    }
    if (m == 0xda || m == 0xd9)
      break;
    if (m == 0xe0 && HAS(c, pos + 4, "JFIF\0") && pos + 11 <= c->len) {
      put(c, ", JFIF standard ");
      put_u(c, c->b[pos + 9]);
      put(c, c->b[pos + 10] < 10 ? ".0" : ".");
      put_u(c, c->b[pos + 10]);
    } else if (m == 0xe1 && HAS(c, pos + 4, "Exif\0")) {
      put(c, ", Exif standard");
    } else if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc && pos + 10 <= c->len) {
      static const char *const modes[] = {", baseline", ", extended sequential", ", progressive", ", lossless"};
      put(c, modes[m & 3]);
      put(c, ", precision ");
      put_u(c, c->b[pos + 4]);
      put_dims(c, be16(c, pos + 7), be16(c, pos + 5));
      put(c, ", components ");
      put_u(c, c->b[pos + 9]);
      break;
    }
    pos += 2 + (int)be16(c, pos + 2);
  }
  return 1;
}

static int gif(struct ctx *c)
{
  if (!HAS(c, 0, "GIF87a") && !HAS(c, 0, "GIF89a"))
    return 0;
  put(c, "GIF image data, version ");
  for (int i = 3; i < 6; i++)
    put_ch(c, (char)c->b[i]);
  if (c->len >= 10)
    put_dims(c, le16(c, 6), le16(c, 8));
  mime(c, "image/gif");
  return 1;
}

static int bmp(struct ctx *c)
{
  if (!HAS(c, 0, "BM") || c->len < 30)
    return 0;
  unsigned hs = le32(c, 14), w, h, bpp, planes;
  const char *fmt;
  if (hs == 12) {
    fmt = "OS/2 1.x format";
    w = le16(c, 18), h = le16(c, 20), planes = le16(c, 22), bpp = le16(c, 24);
  } else {
    if (hs == 40 || hs == 52 || hs == 56)
      fmt = "Windows 3.x format";
    else if (hs == 64)
      fmt = "OS/2 2.x format";
    else if (hs == 108)
      fmt = "Windows 95/NT4 and newer format";
    else if (hs == 124)
      fmt = "Windows 98/2000 and newer format";
    else
      return 0;
    int sh = (int)le32(c, 22);
    w = le32(c, 18), h = sh < 0 ? -(unsigned)sh : (unsigned)sh, planes = le16(c, 26), bpp = le16(c, 28);
  }
  if (planes != 1)
    return 0;
  put(c, "PC bitmap, ");
  put(c, fmt);
  put_dims(c, w, h);
  put(c, " x ");
  put_u(c, bpp);
  mime(c, "image/bmp");
  return 1;
}

static int tiff(struct ctx *c)
{
  if (HAS(c, 0, "II*\0"))
    put(c, "TIFF image data, little-endian");
  else if (HAS(c, 0, "MM\0*"))
    put(c, "TIFF image data, big-endian");
  else
    return 0;
  mime(c, "image/tiff");
  return 1;
}

static int psd(struct ctx *c)
{
  if (!HAS(c, 0, "8BPS") || c->len < 26 || (be16(c, 4) != 1 && be16(c, 4) != 2))
    return 0;
  put(c, be16(c, 4) == 2 ? "Adobe Photoshop Image (PSB)" : "Adobe Photoshop Image");
  put_dims(c, be32(c, 18), be32(c, 14));
  mime(c, "image/vnd.adobe.photoshop");
  return 1;
}

/* Only four fixed bytes, so the first directory entry's reserved byte must be zero as well. */
static int ico(struct ctx *c)
{
  if (c->len < 22 || c->b[0] || c->b[1] || (c->b[2] != 1 && c->b[2] != 2) || c->b[3] || !le16(c, 4) || c->b[9])
    return 0;
  unsigned n = le16(c, 4);
  int cursor = c->b[2] == 2;
  put(c, cursor ? "MS Windows cursor resource - " : "MS Windows icon resource - ");
  put_u(c, n);
  put(c, cursor ? (n == 1 ? " cursor" : " cursors") : (n == 1 ? " icon" : " icons"));
  put_dims(c, c->b[6] ? c->b[6] : 256, c->b[7] ? c->b[7] : 256);
  mime(c, "image/x-icon");
  return 1;
}

static void wav_format(struct ctx *c)
{
  for (int off = 12; off + 8 <= c->len;) {
    unsigned size = le32(c, off + 4);
    if (HAS(c, off, "fmt ") && size >= 16 && off + 24 <= c->len) {
      unsigned tag = le16(c, off + 8);
      switch (tag) {
      case 1:
        put(c, ", Microsoft PCM");
        break;
      case 2:
        put(c, ", Microsoft ADPCM");
        break;
      case 3:
        put(c, ", IEEE float");
        break;
      case 6:
        put(c, ", ITU G.711 A-law");
        break;
      case 7:
        put(c, ", ITU G.711 mu-law");
        break;
      case 0x11:
        put(c, ", IMA ADPCM");
        break;
      case 0x55:
        put(c, ", MPEG Layer 3");
        break;
      case 0xfffe:
        put(c, ", extensible");
        break;
      default:
        put(c, ", format ");
        put_hex(c, tag);
      }
      put(c, ", ");
      put_u(c, le16(c, off + 22));
      put(c, " bit, ");
      put_channels(c, le16(c, off + 10));
      put(c, ", ");
      put_u(c, le32(c, off + 12));
      put(c, " Hz");
      return;
    }
    if (size > (unsigned)c->len)
      return;
    off += 8 + (int)size + (int)(size & 1);
  }
}

static int riff(struct ctx *c)
{
  if (!HAS(c, 0, "RIFF") || c->len < 12)
    return 0;
  put(c, "RIFF (little-endian) data");
  if (HAS(c, 8, "WEBP")) {
    put(c, ", Web/P image");
    mime(c, "image/webp");
    if (HAS(c, 12, "VP8 ") && HAS(c, 23, "\x9d\x01\x2a") && c->len >= 30) {
      put(c, ", VP8 encoding");
      put_dims(c, le16(c, 26) & 0x3fff, le16(c, 28) & 0x3fff);
    } else if (HAS(c, 12, "VP8L") && c->len >= 25 && c->b[20] == 0x2f) {
      unsigned bits = le32(c, 21);
      put(c, ", lossless");
      put_dims(c, (bits & 0x3fff) + 1, (bits >> 14 & 0x3fff) + 1);
    } else if (HAS(c, 12, "VP8X") && c->len >= 30) {
      put(c, ", extended");
      put_dims(c, le24(c, 24) + 1, le24(c, 27) + 1);
      if (c->b[20] & 0x10)
        put(c, ", with alpha");
      if (c->b[20] & 0x02)
        put(c, ", animated");
    }
  } else if (HAS(c, 8, "WAVE")) {
    put(c, ", WAVE audio");
    mime(c, "audio/wav");
    wav_format(c);
  } else if (HAS(c, 8, "AVI ")) {
    put(c, ", AVI");
    mime(c, "video/x-msvideo");
  } else {
    mime(c, "application/octet-stream");
  }
  return 1;
}

static int flac(struct ctx *c)
{
  if (!HAS(c, 0, "fLaC"))
    return 0;
  put(c, "FLAC audio bitstream data");
  mime(c, "audio/flac");
  if (c->len >= 22 && (c->b[4] & 0x7f) == 0) {
    const u8 *s = c->b + 18;
    unsigned rate = (unsigned)s[0] << 12 | (unsigned)s[1] << 4 | s[2] >> 4;
    unsigned chans = (s[2] >> 1 & 7) + 1, bps = ((s[2] & 1) << 4 | s[3] >> 4) + 1;
    put(c, ", ");
    put_u(c, bps);
    put(c, " bit, ");
    put_channels(c, chans);
    put(c, ", ");
    put_u(c, rate);
    put(c, " Hz");
  }
  return 1;
}

static int ogg(struct ctx *c)
{
  if (!HAS(c, 0, "OggS") || c->len < 27)
    return 0;
  int p = 27 + c->b[26];
  put(c, "Ogg data");
  mime(c, "application/ogg");
  if (HAS(c, p, "\x01vorbis") && p + 16 <= c->len) {
    put(c, ", Vorbis audio, ");
    put_channels(c, c->b[p + 11]);
    put(c, ", ");
    put_u(c, le32(c, p + 12));
    put(c, " Hz");
    mime(c, "audio/ogg");
  } else if (HAS(c, p, "OpusHead") && p + 16 <= c->len) {
    put(c, ", Opus audio, ");
    put_channels(c, c->b[p + 9]);
    put(c, ", ");
    put_u(c, le32(c, p + 12));
    put(c, " Hz");
    mime(c, "audio/ogg");
  } else if (HAS(c, p,
                 "\x7f"
                 "FLAC")) {
    put(c, ", FLAC audio");
    mime(c, "audio/ogg");
  } else if (HAS(c, p, "Speex   ")) {
    put(c, ", Speex audio");
    mime(c, "audio/ogg");
  } else if (HAS(c, p, "\x80theora")) {
    put(c, ", Theora video");
    mime(c, "video/ogg");
  }
  return 1;
}

static int id3(struct ctx *c)
{
  if (!HAS(c, 0, "ID3") || c->len < 10 || c->b[3] == 0xff || c->b[4] == 0xff)
    return 0;
  put(c, "Audio file with ID3 version 2.");
  put_u(c, c->b[3]);
  put(c, ".");
  put_u(c, c->b[4]);
  mime(c, "audio/mpeg");
  return 1;
}

/* Bare MPEG audio frames: an 11-bit sync word only, so the header's reserved values must be absent. */
static int mpeg_audio(struct ctx *c)
{
  if (c->len < 4 || c->b[0] != 0xff || (c->b[1] & 0xe0) != 0xe0)
    return 0;
  unsigned b1 = c->b[1], b2 = c->b[2], b3 = c->b[3];
  unsigned layer = b1 >> 1 & 3, ver = b1 >> 3 & 3;
  if (layer == 0) {
    static const unsigned aac_rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                         22050, 16000, 12000, 11025, 8000,  7350};
    unsigned ri = b2 >> 2 & 15;
    if ((b1 & 0xf6) != 0xf0 || ri > 12)
      return 0;
    put(c, b1 & 8 ? "MPEG ADTS, AAC, v2, " : "MPEG ADTS, AAC, v4, ");
    put_u(c, aac_rates[ri]);
    put(c, " Hz, ");
    put_channels(c, (b2 & 1) << 2 | b3 >> 6);
    mime(c, "audio/aac");
    return 1;
  }
  unsigned ri = b2 >> 2 & 3;
  if (ver == 1 || ri == 3 || b2 >> 4 == 15)
    return 0;
  static const unsigned v1_rates[] = {44100, 48000, 32000};
  static const char *const layers[] = {0, "III", "II", "I"}, *const vers[] = {"2.5", 0, "2", "1"};
  static const char *const modes[] = {"stereo", "joint stereo", "dual mono", "mono"};
  put(c, "MPEG ADTS, layer ");
  put(c, layers[layer]);
  put(c, ", v");
  put(c, vers[ver]);
  put(c, ", ");
  put_u(c, v1_rates[ri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2));
  put(c, " Hz, ");
  put(c, modes[b3 >> 6]);
  mime(c, "audio/mpeg");
  return 1;
}

static int midi(struct ctx *c)
{
  if (!HAS(c, 0, "MThd\0\0\0\x06") || c->len < 14)
    return 0;
  put(c, "Standard MIDI data (format ");
  put_u(c, be16(c, 8));
  put(c, ") using ");
  put_u(c, be16(c, 10));
  put(c, be16(c, 10) == 1 ? " track" : " tracks");
  mime(c, "audio/midi");
  return 1;
}

static int ftyp(struct ctx *c)
{
  static const struct {
    const char *brand, *desc, *mime;
  } brands[] = {
      {"isom", "MP4 Base Media v1", "video/mp4"},
      {"iso2", "MP4 Base Media v2", "video/mp4"},
      {"mp41", "MP4 v1", "video/mp4"},
      {"mp42", "MP4 v2", "video/mp4"},
      {"avc1", "MP4 Base w/ AVC ext", "video/mp4"},
      {"dash", "MPEG-DASH media", "video/mp4"},
      {"M4A ", "Apple iTunes AAC-LC (.M4A) Audio", "audio/mp4"},
      {"M4B ", "Apple iTunes AAC-LC (.M4B) Audio Book", "audio/mp4"},
      {"M4V ", "Apple iTunes Video (.M4V)", "video/x-m4v"},
      {"qt  ", "Apple QuickTime movie", "video/quicktime"},
      {"heic", "HEIF image (HEVC)", "image/heic"},
      {"heix", "HEIF image (HEVC)", "image/heic"},
      {"mif1", "HEIF image", "image/heif"},
      {"avif", "AVIF image", "image/avif"},
      {"avis", "AVIF image sequence", "image/avif"},
  };
  if (!HAS(c, 4, "ftyp") || be32(c, 0) < 12 || c->len < 12)
    return 0;
  put(c, "ISO Media, ");
  for (unsigned i = 0; i < sizeof brands / sizeof brands[0]; i++) {
    if (has(c, 8, brands[i].brand, 4)) {
      put(c, brands[i].desc);
      mime(c, brands[i].mime);
      return 1;
    }
  }
  if (HAS(c, 8, "3gp")) {
    put(c, "3GPP");
    mime(c, "video/3gpp");
    return 1;
  }
  put_label(c, "brand ", 8, 4);
  mime(c, "application/octet-stream");
  return 1;
}

static int ebml(struct ctx *c)
{
  if (!HAS(c, 0, "\x1a\x45\xdf\xa3"))
    return 0;
  int end = c->len < 64 ? c->len : 64;
  for (int i = 4; i + 3 <= end; i++) {
    if (c->b[i] != 0x42 || c->b[i + 1] != 0x82 || !(c->b[i + 2] & 0x80))
      continue;
    int n = c->b[i + 2] & 0x7f;
    if (n == 4 && HAS(c, i + 3, "webm")) {
      put(c, "WebM");
      mime(c, "video/webm");
    } else if (n == 8 && HAS(c, i + 3, "matroska")) {
      put(c, "Matroska data");
      mime(c, "video/x-matroska");
    } else {
      put(c, "EBML file");
      put_label(c, ", DocType ", i + 3, n);
      mime(c, "application/octet-stream");
    }
    return 1;
  }
  put(c, "EBML file");
  mime(c, "application/octet-stream");
  return 1;
}

static int pdf(struct ctx *c)
{
  if (!HAS(c, 0, "%PDF-"))
    return 0;
  put(c, "PDF document");
  int i = 5;
  if (i < c->len && c->b[i] >= '0' && c->b[i] <= '9') {
    put(c, ", version ");
    for (; i < c->len && i < 9 && ((c->b[i] >= '0' && c->b[i] <= '9') || c->b[i] == '.'); i++)
      put_ch(c, (char)c->b[i]);
  }
  mime(c, "application/pdf");
  return 1;
}

static int sqlite(struct ctx *c)
{
  if (!HAS(c, 0, "SQLite format 3\0"))
    return 0;
  put(c, "SQLite 3.x database");
  mime(c, "application/vnd.sqlite3");
  return 1;
}

static int font(struct ctx *c)
{
  if (HAS(c, 0, "wOFF")) {
    put(c, "Web Open Font Format");
    mime(c, "font/woff");
  } else if (HAS(c, 0, "wOF2")) {
    put(c, "Web Open Font Format (Version 2)");
    mime(c, "font/woff2");
  } else if (HAS(c, 0, "ttcf") && c->len >= 12) {
    put(c, "TrueType font collection data, ");
    put_u(c, be32(c, 8));
    put(c, be32(c, 8) == 1 ? " font" : " fonts");
    mime(c, "font/collection");
  } else if (HAS(c, 0, "OTTO") && be16(c, 4) >= 1 && be16(c, 4) <= 100) {
    put(c, "OpenType font data, ");
    put_u(c, be16(c, 4));
    put(c, " tables");
    mime(c, "font/otf");
  } else if ((HAS(c, 0, "\0\1\0\0") || HAS(c, 0, "true")) && be16(c, 4) >= 1 && be16(c, 4) <= 100) {
    put(c, "TrueType Font data, ");
    put_u(c, be16(c, 4));
    put(c, " tables");
    mime(c, "font/ttf");
  } else {
    return 0;
  }
  return 1;
}

static int zip(struct ctx *c)
{
  static const struct {
    const char *mime, *desc;
  } stored[] = {
      {"application/epub+zip", "EPUB document"},
      {"application/vnd.oasis.opendocument.text", "OpenDocument Text"},
      {"application/vnd.oasis.opendocument.spreadsheet", "OpenDocument Spreadsheet"},
      {"application/vnd.oasis.opendocument.presentation", "OpenDocument Presentation"},
      {"application/vnd.oasis.opendocument.graphics", "OpenDocument Drawing"},
  };
  if (HAS(c, 0, "PK\5\6")) {
    put(c, "Zip archive data (empty)");
    mime(c, "application/zip");
    return 1;
  }
  if (HAS(c, 0, "PK\7\x08")) {
    put(c, "Zip multi-volume archive data");
    mime(c, "application/zip");
    return 1;
  }
  if (!HAS(c, 0, "PK\3\4") || c->len < 30)
    return 0;
  unsigned ver = le16(c, 4), nlen = le16(c, 26), data = 30 + nlen + le16(c, 28);
  /* EPUB and ODF store their MIME type uncompressed as the first member, named "mimetype". */
  if (nlen == 8 && HAS(c, 30, "mimetype") && le16(c, 8) == 0) {
    for (unsigned i = 0; i < sizeof stored / sizeof stored[0]; i++) {
      int n = slen(stored[i].mime);
      if (le32(c, 18) == (unsigned)n && has(c, (int)data, stored[i].mime, n)) {
        put(c, stored[i].desc);
        mime(c, stored[i].mime);
        return 1;
      }
    }
  }
  if (nlen == 19 && HAS(c, 30, "[Content_Types].xml")) {
    if (contains(c, 30, "word/")) {
      put(c, "Microsoft Word 2007+");
      mime(c, "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    } else if (contains(c, 30, "xl/")) {
      put(c, "Microsoft Excel 2007+");
      mime(c, "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
    } else if (contains(c, 30, "ppt/")) {
      put(c, "Microsoft PowerPoint 2007+");
      mime(c, "application/vnd.openxmlformats-officedocument.presentationml.presentation");
    } else {
      put(c, "Microsoft OOXML");
      mime(c, "application/zip");
    }
    return 1;
  }
  if (HAS(c, 30, "META-INF/")) {
    put(c, "Java archive data (JAR)");
    mime(c, "application/java-archive");
    return 1;
  }
  put(c, "Zip archive data, at least v");
  put_u(c, ver / 10);
  put(c, ".");
  put_u(c, ver % 10);
  put(c, " to extract");
  mime(c, "application/zip");
  return 1;
}

static int gzip(struct ctx *c)
{
  if (!HAS(c, 0, "\x1f\x8b") || c->len < 10)
    return 0;
  put(c, "gzip compressed data");
  mime(c, "application/gzip");
  if (c->b[2] != 8) {
    put(c, ", unknown method");
    return 1;
  }
  unsigned flags = c->b[3];
  int off = 10;
  if (flags & 4)
    off += 2 + (int)le16(c, 10);
  if (flags & 8 && off < c->len) {
    put(c, ", was \"");
    for (int i = off; i < c->len && i < off + 64 && c->b[i]; i++)
      put_ch(c, printable(c->b[i]) ? (char)c->b[i] : '?');
    put(c, "\"");
  }
  switch (c->b[9]) {
  case 0:
    put(c, ", from FAT filesystem (MS-DOS, OS/2, NT)");
    break;
  case 3:
    put(c, ", from Unix");
    break;
  case 7:
    put(c, ", from Macintosh");
    break;
  case 11:
    put(c, ", from NTFS filesystem (NT)");
    break;
  default:
    break;
  }
  if (c->complete && c->len >= 18) {
    put(c, ", original size modulo 2^32 ");
    put_u(c, le32(c, c->len - 4));
  }
  return 1;
}

static int compressed(struct ctx *c)
{
  if (HAS(c, 0, "BZh") && c->len >= 4 && c->b[3] >= '1' && c->b[3] <= '9') {
    put(c, "bzip2 compressed data, block size = ");
    put_ch(c, (char)c->b[3]);
    put(c, "00k");
    mime(c, "application/x-bzip2");
  } else if (HAS(c, 0,
                 "\xfd"
                 "7zXZ\0") &&
             c->len >= 8) {
    static const char *const checks[16] = {"NONE", "CRC32", 0, 0, "CRC64", 0, 0, 0, 0, 0, "SHA-256"};
    const char *check = checks[c->b[7] & 15];
    put(c, "XZ compressed data");
    if (check) {
      put(c, ", checksum ");
      put(c, check);
    }
    mime(c, "application/x-xz");
  } else if (HAS(c, 0, "\x28\xb5\x2f\xfd")) {
    put(c, "Zstandard compressed data (v0.8+)");
    mime(c, "application/zstd");
  } else if (HAS(c, 0, "\x04\x22\x4d\x18")) {
    put(c, "LZ4 compressed data (v1.4+)");
    mime(c, "application/x-lz4");
  } else if (HAS(c, 0, "7z\xbc\xaf\x27\x1c") && c->len >= 8) {
    put(c, "7-zip archive data, version ");
    put_u(c, c->b[6]);
    put(c, ".");
    put_u(c, c->b[7]);
    mime(c, "application/x-7z-compressed");
  } else if (HAS(c, 0, "Rar!\x1a\x07\x00")) {
    put(c, "RAR archive data, v4");
    mime(c, "application/vnd.rar");
  } else if (HAS(c, 0, "Rar!\x1a\x07\x01\x00")) {
    put(c, "RAR archive data, v5");
    mime(c, "application/vnd.rar");
  } else {
    return 0;
  }
  return 1;
}

static int archive(struct ctx *c)
{
  if (HAS(c, 0, "!<arch>\n")) {
    if (HAS(c, 8, "debian-binary")) {
      put(c, "Debian binary package");
      mime(c, "application/vnd.debian.binary-package");
    } else {
      put(c, "current ar archive");
      mime(c, "application/x-archive");
    }
  } else if (HAS(c, 0, "070701")) {
    put(c, "ASCII cpio archive (SVR4 with no CRC)");
    mime(c, "application/x-cpio");
  } else if (HAS(c, 0, "070702")) {
    put(c, "ASCII cpio archive (SVR4 with CRC)");
    mime(c, "application/x-cpio");
  } else if (HAS(c, 0, "070707")) {
    put(c, "ASCII cpio archive (pre-SVR4 or odc)");
    mime(c, "application/x-cpio");
  } else if (HAS(c, 257,
                 "ustar\0"
                 "00")) {
    put(c, "POSIX tar archive");
    mime(c, "application/x-tar");
  } else if (HAS(c, 257, "ustar  \0")) {
    put(c, "POSIX tar archive (GNU)");
    mime(c, "application/x-tar");
  } else {
    return 0;
  }
  return 1;
}

static int iso9660(struct ctx *c)
{
  if (!HAS(c, 0x8001, "CD001"))
    return 0;
  put(c, "ISO 9660 CD-ROM filesystem data");
  if (HAS(c, 510, "\x55\xaa"))
    put(c, " (DOS/MBR boot sector)");
  put_label(c, " ", 0x8028, 32);
  if (c->len > 0x8800 && c->b[0x8800] == 0 && HAS(c, 0x8807, "EL TORITO SPECIFICATION"))
    put(c, " (bootable)");
  mime(c, "application/x-iso9660-image");
  return 1;
}

static int btrfs(struct ctx *c)
{
  if (!HAS(c, 0x10040, "_BHRfS_M"))
    return 0;
  put(c, "BTRFS filesystem data");
  put_label(c, ", label ", 0x1012b, 256);
  mime(c, "application/octet-stream");
  return 1;
}

static int ext(struct ctx *c)
{
  const int sb = 1024;
  if (le16(c, sb + 0x38) != 0xef53 || c->len < sb + 0x88)
    return 0;
  unsigned compat = le32(c, sb + 0x5c), incompat = le32(c, sb + 0x60);
  put(c, "Linux rev ");
  put_u(c, le32(c, sb + 0x4c));
  put(c, ".");
  put_u(c, le16(c, sb + 0x3e));
  /* extents, 64bit or flex_bg mean ext4; a journal without them means ext3. */
  put(c, incompat & 0x2c0 ? " ext4" : compat & 4 ? " ext3" : " ext2");
  put(c, " filesystem data");
  put_label(c, ", volume name ", sb + 0x78, 16);
  mime(c, "application/octet-stream");
  return 1;
}

static int qcow(struct ctx *c)
{
  if (!HAS(c, 0, "QFI\xfb") || c->len < 32)
    return 0;
  unsigned v = be32(c, 4);
  put(c, v >= 2 ? "QEMU QCOW2 Image (v" : "QEMU QCOW Image (v");
  put_u(c, v);
  put(c, "), ");
  put_u64(c, be64(c, 24));
  put(c, " bytes");
  mime(c, "application/x-qemu-disk");
  return 1;
}

/* The native filesystem's superblock: "8OWO"/"9OWO", the directory extent, then zero padding to 512. */
static int owofs(struct ctx *c)
{
  if (c->len < 512 || !HAS(c, 1, "OWO") || (c->b[0] != '8' && c->b[0] != '9'))
    return 0;
  if (!le32(c, 4) || !le32(c, 8))
    return 0;
  for (int i = 12; i < 512; i++)
    if (c->b[i])
      return 0;
  put(c, "OwOS filesystem data, version ");
  put_ch(c, (char)c->b[0]);
  mime(c, "application/octet-stream");
  return 1;
}

static int boot_sector(struct ctx *c)
{
  if (!HAS(c, 510, "\x55\xaa"))
    return 0;
  unsigned bps = le16(c, 11);
  int vbr = (c->b[0] == 0xeb || c->b[0] == 0xe9) && (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096);
  put(c, "DOS/MBR boot sector");
  mime(c, "application/octet-stream");
  if (vbr && HAS(c, 82, "FAT32   ")) {
    put(c, ", FAT (32 bit)");
    put_label(c, ", label: ", 71, 11);
  } else if (vbr && (HAS(c, 54, "FAT12   ") || HAS(c, 54, "FAT16   "))) {
    put(c, HAS(c, 54, "FAT12") ? ", FAT (12 bit)" : ", FAT (16 bit)");
    put_label(c, ", label: ", 43, 11);
  } else if (HAS(c, 3, "NTFS    ")) {
    put(c, ", NTFS");
  } else if (HAS(c, 3, "EXFAT   ")) {
    put(c, ", exFAT");
  } else if (HAS(c, 512, "EFI PART")) {
    put(c, "; GPT partition table");
  } else {
    int parts = 0;
    for (int i = 0; i < 4; i++) {
      u8 boot = c->b[446 + i * 16];
      if (boot != 0 && boot != 0x80)
        return 1;
      parts += c->b[446 + i * 16 + 4] != 0;
    }
    if (parts) {
      put(c, ", ");
      put_u(c, (unsigned)parts);
      put(c, parts == 1 ? " partition" : " partitions");
    }
  }
  return 1;
}

static int is_space(u8 ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; }

static int skip_ws(const struct ctx *c, int p)
{
  while (p < c->len && is_space(c->b[p]))
    p++;
  return p;
}

/* Control characters that still count as text: BEL, BS, TAB, LF, VT, FF, CR and ESC. */
static int text_ctl(unsigned ch) { return (ch >= 7 && ch <= 13) || ch == 27; }

enum enc { ENC_ASCII, ENC_UTF8, ENC_ISO8859, ENC_EXTASCII, ENC_UTF16LE, ENC_UTF16BE };

/* Byte length of the valid UTF-8 sequence at p, 0 if invalid, -1 if cut off by the end of the data. */
static int utf8_seq(const struct ctx *c, int p)
{
  u8 ch = c->b[p];
  int n;
  u8 lo = 0x80, hi = 0xbf;
  if (ch >= 0xc2 && ch <= 0xdf) {
    n = 1;
  } else if (ch >= 0xe0 && ch <= 0xef) {
    n = 2;
    if (ch == 0xe0)
      lo = 0xa0;
    if (ch == 0xed)
      hi = 0x9f;
  } else if (ch >= 0xf0 && ch <= 0xf4) {
    n = 3;
    if (ch == 0xf0)
      lo = 0x90;
    if (ch == 0xf4)
      hi = 0x8f;
  } else {
    return 0;
  }
  for (int i = 1; i <= n; i++) {
    if (p + i >= c->len)
      return -1;
    u8 cc = c->b[p + i];
    if (cc < (i == 1 ? lo : 0x80) || cc > (i == 1 ? hi : 0xbf))
      return 0;
  }
  return n + 1;
}

/* Classify the bytes from `start` as text; 0 when they are binary. */
static int text_encoding(const struct ctx *c, int start, enum enc *out)
{
  if (HAS(c, 0, "\xff\xfe") || HAS(c, 0, "\xfe\xff")) {
    int le = c->b[0] == 0xff;
    if (c->complete && (c->len & 1))
      return 0;
    for (int i = 2; i + 1 < c->len; i += 2) {
      unsigned u = le ? le16(c, i) : be16(c, i);
      if ((u < 0x20 && !text_ctl(u)) || u == 0x7f)
        return 0;
    }
    *out = le ? ENC_UTF16LE : ENC_UTF16BE;
    return 1;
  }
  int high = 0, utf8 = 1, c1 = 0;
  for (int i = start; i < c->len; i++) {
    u8 ch = c->b[i];
    if (ch < 0x80) {
      if ((ch < 0x20 && !text_ctl(ch)) || ch == 0x7f)
        return 0;
      continue;
    }
    high = 1;
    c1 |= ch < 0xa0;
    if (utf8) {
      int n = utf8_seq(c, i);
      if (n > 0)
        i += n - 1;
      else if (n < 0 && !c->complete)
        break;
      else
        utf8 = 0;
    }
  }
  if (!high)
    *out = ENC_ASCII;
  else if (utf8)
    *out = ENC_UTF8;
  else
    *out = c1 ? ENC_EXTASCII : ENC_ISO8859;
  return 1;
}

static const struct {
  const char *name, *desc, *mime;
  int prefix; /* match any interpreter starting with name, like python3.12 */
} interpreters[] = {
    {"sh", "POSIX shell script", "text/x-shellscript", 0},
    {"dash", "POSIX shell script", "text/x-shellscript", 0},
    {"ash", "POSIX shell script", "text/x-shellscript", 0},
    {"bash", "Bourne-Again shell script", "text/x-shellscript", 0},
    {"zsh", "Paul Falstad's zsh script", "text/x-shellscript", 0},
    {"python", "Python script", "text/x-script.python", 1},
    {"perl", "Perl script", "text/x-perl", 0},
    {"ruby", "Ruby script", "text/x-ruby", 0},
    {"node", "Node.js script", "application/javascript", 0},
    {"lua", "Lua script", "text/x-lua", 1},
    {"awk", "awk script", "text/x-awk", 0},
    {"gawk", "GNU awk script", "text/x-awk", 0},
    {"make", "makefile script", "text/x-makefile", 0},
};

/* Describe a "#!" line at p: names the interpreter, or `env`'s first non-option argument. */
static void script(struct ctx *c, int p)
{
  int s = 0, e = 0;
  p += 2;
  for (int round = 0; round < 2; round++) {
    while (p < c->len && (c->b[p] == ' ' || c->b[p] == '\t'))
      p++;
    s = p;
    while (p < c->len && !is_space(c->b[p]))
      p++;
    e = p;
    int base = s;
    for (int i = s; i < e; i++)
      if (c->b[i] == '/')
        base = i + 1;
    if (round == 0 && e - base == 3 && has(c, base, "env", 3)) {
      while (p < c->len && (c->b[p] == ' ' || c->b[p] == '\t'))
        p++;
      while (p < c->len && c->b[p] == '-') {
        while (p < c->len && !is_space(c->b[p]))
          p++;
        while (p < c->len && (c->b[p] == ' ' || c->b[p] == '\t'))
          p++;
      }
      continue;
    }
    s = base;
    break;
  }
  for (unsigned i = 0; i < sizeof interpreters / sizeof interpreters[0]; i++) {
    int n = slen(interpreters[i].name);
    if ((e - s == n || (interpreters[i].prefix && e - s > n)) && has(c, s, interpreters[i].name, n)) {
      put(c, interpreters[i].desc);
      mime(c, interpreters[i].mime);
      return;
    }
  }
  put(c, "a ");
  for (int i = s; i < e && i < s + 64; i++)
    put_ch(c, printable(c->b[i]) ? (char)c->b[i] : '?');
  put(c, " script");
}

static int looks_json(const struct ctx *c, int p)
{
  if (!c->complete || p >= c->len || (c->b[p] != '{' && c->b[p] != '['))
    return 0;
  u8 open = c->b[p];
  int e = c->len - 1;
  while (e > p && is_space(c->b[e]))
    e--;
  if (c->b[e] != (open == '{' ? '}' : ']'))
    return 0;
  int q = skip_ws(c, p + 1);
  u8 n = c->b[q];
  if (open == '{')
    return n == '"' || n == '}';
  return n == '"' || n == '{' || n == '[' || n == ']' || n == '-' || (n >= '0' && n <= '9') || n == 't' || n == 'f' ||
         n == 'n';
}

/* Formats whose text body is not worth an encoding suffix, as file(1) prints them. */
static int text_document(struct ctx *c, int p)
{
  static const struct {
    const char *label, *desc;
  } pem[] = {
      {"CERTIFICATE-----", "PEM certificate"},
      {"CERTIFICATE REQUEST-----", "PEM certificate request"},
      {"OPENSSH PRIVATE KEY-----", "OpenSSH private key"},
      {"PGP PUBLIC KEY BLOCK-----", "PGP public key block"},
      {"PGP SIGNATURE-----", "PGP signature"},
      {"PGP MESSAGE-----", "PGP message"},
  };
  static const struct {
    const char *prefix, *desc;
  } ssh_keys[] = {
      {"ssh-ed25519 ", "OpenSSH ED25519 public key"},
      {"ssh-rsa ", "OpenSSH RSA public key"},
      {"ecdsa-sha2-nistp", "OpenSSH ECDSA public key"},
  };
  if (HAS(c, p, "%!PS")) {
    put(c, "PostScript document text");
    mime(c, "application/postscript");
    return 1;
  }
  if (HAS(c, p, "-----BEGIN ")) {
    int l = p + 11;
    for (unsigned i = 0; i < sizeof pem / sizeof pem[0]; i++) {
      if (has(c, l, pem[i].label, slen(pem[i].label))) {
        put(c, pem[i].desc);
        mime(c, "text/plain");
        return 1;
      }
    }
    for (int i = l; i + 16 <= c->len && c->b[i] != '\n'; i++) {
      if (HAS(c, i, "PRIVATE KEY-----") || HAS(c, i, "PUBLIC KEY-----")) {
        put(c, HAS(c, i, "PRIVATE") ? "PEM private key" : "PEM public key");
        mime(c, "text/plain");
        return 1;
      }
    }
  }
  for (unsigned i = 0; i < sizeof ssh_keys / sizeof ssh_keys[0]; i++) {
    if (has(c, p, ssh_keys[i].prefix, slen(ssh_keys[i].prefix))) {
      put(c, ssh_keys[i].desc);
      mime(c, "text/plain");
      return 1;
    }
  }
  if (looks_json(c, p)) {
    put(c, "JSON text data");
    mime(c, "application/json");
    return 1;
  }
  if (HAS(c, p, "<svg") || (HAS(c, p, "<?xml") && contains(c, p, "<svg"))) {
    put(c, "SVG Scalable Vector Graphics image");
    mime(c, "image/svg+xml");
    return 1;
  }
  return 0;
}

/* The text kind that leads the description ("HTML document, "), or 0 when it is plain text. */
static int text_kind(struct ctx *c, int start, int *exec)
{
  if (HAS(c, start, "#!")) {
    script(c, start);
    *exec = 1;
    return 1;
  }
  int p = skip_ws(c, start);
  if (HAS(c, p, "<?xml")) {
    put(c, HAS(c, p + 5, " version=\"1.0\"") ? "XML 1.0 document" : "XML document");
    mime(c, "text/xml");
  } else if (has_ci(c, p, "<!doctype html") || has_ci(c, p, "<html")) {
    put(c, "HTML document");
    mime(c, "text/html");
  } else if (HAS(c, p, "[Desktop Entry]")) {
    put(c, "desktop entry");
    mime(c, "application/x-desktop");
  } else {
    return 0;
  }
  return 1;
}

static int text(struct ctx *c)
{
  static const char *const names[] = {"ASCII",
                                      "Unicode text, UTF-8",
                                      "ISO-8859",
                                      "Non-ISO extended-ASCII",
                                      "Unicode text, UTF-16, little-endian",
                                      "Unicode text, UTF-16, big-endian"};
  static const char *const charsets[] = {"us-ascii", "utf-8", "iso-8859-1", "unknown-8bit", "utf-16le", "utf-16be"};
  int bom = HAS(c, 0, "\xef\xbb\xbf"), start = bom ? 3 : 0, exec = 0;
  enum enc enc;
  if (!text_encoding(c, start, &enc))
    return 0;
  int wide = enc == ENC_UTF16LE || enc == ENC_UTF16BE;
  if (bom && enc == ENC_ASCII)
    enc = ENC_UTF8;
  c->mi->charset = charsets[enc];
  mime(c, "text/plain");

  if (!wide) {
    if (text_document(c, skip_ws(c, start)))
      return 1;
    if (text_kind(c, start, &exec))
      put(c, ", ");
  }
  put(c, names[enc]);
  if (bom)
    put(c, " (with BOM)");
  put(c, exec ? " text executable" : " text");

  int crlf = 0, cr = 0, lf = 0, esc = 0, bs = 0, step = wide ? 2 : 1;
  for (int i = wide ? 2 : start; i + step <= c->len; i += step) {
    unsigned u = wide ? (enc == ENC_UTF16LE ? le16(c, i) : be16(c, i)) : c->b[i];
    unsigned next = i + 2 * step <= c->len
                        ? (wide ? (enc == ENC_UTF16LE ? le16(c, i + step) : be16(c, i + step)) : c->b[i + step])
                        : 0;
    if (u == '\r' && next == '\n') {
      crlf = 1;
      i += step;
    } else if (u == '\r') {
      cr = 1;
    } else if (u == '\n') {
      lf = 1;
    } else if (u == 27) {
      esc = 1;
    } else if (u == 8) {
      bs = 1;
    }
  }
  if (crlf || cr) {
    const char *sep = "";
    put(c, ", with ");
    if (crlf) {
      put(c, "CRLF");
      sep = ", ";
    }
    if (cr) {
      put(c, sep);
      put(c, "CR");
      sep = ", ";
    }
    if (lf) {
      put(c, sep);
      put(c, "LF");
    }
    put(c, " line terminators");
  } else if (!lf && c->complete) {
    put(c, ", with no line terminators");
  }
  if (esc)
    put(c, ", with escape sequences");
  if (bs)
    put(c, ", with overstriking");
  return 1;
}

/* The UTF-16LE byte-order mark FF FE is also an MPEG Layer I sync word; text that decodes cleanly wins. */
static int utf16le_text(struct ctx *c) { return HAS(c, 0, "\xff\xfe") && text(c); }

/* Strong magic first; signatures of only a few bytes (bare MPEG frames, icons, bitmaps) and the
 * text heuristics run last so they cannot shadow a better match. */
static int (*const detectors[])(struct ctx *) = {
    elf,     pe,    macho, cafebabe, wasm,  png,         jpeg,         gif,        tiff, psd,  riff,       flac,
    ogg,     id3,   midi,  ftyp,     ebml,  pdf,         sqlite,       font,       zip,  gzip, compressed, archive,
    iso9660, btrfs, ext,   qcow,     owofs, boot_sector, utf16le_text, mpeg_audio, ico,  bmp,  text,
};

void magic_identify(const void *data, int len, int complete, struct magic_info *out)
{
  struct ctx c = {(const u8 *)data, len < 0 ? 0 : len, complete, out, 0};
  out->desc[0] = 0;
  out->mime = "application/octet-stream";
  out->charset = "binary";
  if (c.len == 0) {
    put(&c, "empty");
    out->mime = "inode/x-empty";
    return;
  }
  for (unsigned i = 0; i < sizeof detectors / sizeof detectors[0]; i++) {
    if (detectors[i](&c))
      return;
    /* A detector that bails out after writing would leave a partial description behind. */
    c.n = 0;
    out->desc[0] = 0;
    out->mime = "application/octet-stream";
    out->charset = "binary";
  }
  put(&c, "data");
}
