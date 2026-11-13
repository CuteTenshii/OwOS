/* Host test for file(1)'s content identification (user/lib/mime/magic.c). */
#define OWOS_SUITE_TEST
#include "magic.h"
#include "test.h"
#include <string.h>

static struct magic_info mi;
static unsigned char buf[MAGIC_PEEK];

static const char *ident(const void *data, int len, int complete)
{
  magic_identify(data, len, complete, &mi);
  return mi.desc;
}

static const char *ident_str(const char *s) { return ident(s, (int)strlen(s), 1); }

static void put16le(unsigned char *p, unsigned v)
{
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
}

static void put32le(unsigned char *p, unsigned v)
{
  put16le(p, v);
  put16le(p + 2, v >> 16);
}

static void put32be(unsigned char *p, unsigned v)
{
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

static void check_elf(void)
{
  memset(buf, 0, 128);
  memcpy(buf, "\177ELF\1\1\1\0", 8);
  put16le(buf + 16, 2);
  put16le(buf + 18, 3);
  put32le(buf + 28, 52);
  put16le(buf + 42, 32);
  put16le(buf + 44, 1);
  put32le(buf + 52, 1); /* PT_LOAD */
  expect_string(ident(buf, 84, 0), "ELF 32-bit LSB executable, Intel 80386, version 1 (SYSV), statically linked");
  expect_string(mi.mime, "application/x-executable");
  expect_string(mi.charset, "binary");

  memset(buf, 0, 256);
  memcpy(buf, "\177ELF\2\1\1\3", 8);
  put16le(buf + 16, 3);
  put16le(buf + 18, 62);
  put32le(buf + 32, 64);
  put16le(buf + 54, 56);
  put16le(buf + 56, 2);
  put32le(buf + 64, 6);      /* PT_PHDR */
  put32le(buf + 64 + 56, 3); /* PT_INTERP */
  expect_string(ident(buf, 200, 0), "ELF 64-bit LSB pie executable, x86-64, version 1 (GNU/Linux), dynamically linked");
  expect_string(mi.mime, "application/x-pie-executable");

  put32le(buf + 64 + 56, 1);
  expect_string(ident(buf, 200, 0), "ELF 64-bit LSB shared object, x86-64, version 1 (GNU/Linux)");
}

static void check_pe(void)
{
  memset(buf, 0, 512);
  memcpy(buf, "MZ", 2);
  put32le(buf + 0x3c, 0x80);
  memcpy(buf + 0x80, "PE\0\0", 4);
  put16le(buf + 0x84, 0x8664);
  put16le(buf + 0x80 + 24, 0x20b);
  put16le(buf + 0x80 + 24 + 68, 10);
  expect_string(ident(buf, 512, 1), "PE32+ executable (EFI application) x86-64");
  expect_string(mi.mime, "application/vnd.microsoft.portable-executable");

  put32le(buf + 0x3c, 0xfffffff0u);
  expect_string(ident(buf, 512, 1), "MS-DOS executable");
}

static void check_images(void)
{
  static const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0,    0, 0, 13, 'I', 'H', 'D',
                                      'R',  0,   0,   2,   0x80, 0,    0,    1,    0xe0, 8, 6, 0,  0,   0};
  expect_string(ident(png, sizeof png, 0), "PNG image data, 640 x 480, 8-bit/color RGBA, non-interlaced");
  expect_string(mi.mime, "image/png");

  static const unsigned char jpg[] = {0xff, 0xd8, 0xff, 0xe0, 0, 16,   'J',  'F', 'I', 'F', 0,    1,    1,    0,    0,
                                      1,    0,    1,    0,    0, 0xff, 0xc2, 0,   17,  8,   0x01, 0xe0, 0x02, 0x80, 3};
  expect_string(ident(jpg, sizeof jpg, 0),
                "JPEG image data, JFIF standard 1.01, progressive, precision 8, 640 x 480, components 3");

  expect_string(ident("GIF89a\x40\x01\xc8\x00", 10, 0), "GIF image data, version 89a, 320 x 200");

  memset(buf, 0, 64);
  memcpy(buf, "BM", 2);
  put32le(buf + 14, 40);
  put32le(buf + 18, 16);
  put32le(buf + 22, (unsigned)-8);
  put16le(buf + 26, 1);
  put16le(buf + 28, 24);
  expect_string(ident(buf, 64, 1), "PC bitmap, Windows 3.x format, 16 x 8 x 24");
  put16le(buf + 26, 7);
  expect_string(ident(buf, 64, 1), "data");

  static const unsigned char webp[] = {'R', 'I', 'F', 'F', 0, 0,    0, 0, 'W', 'E',  'B',  'P', 'V',  'P',  '8',
                                       'X', 10,  0,   0,   0, 0x12, 0, 0, 0,   0x7f, 0x02, 0,   0xdf, 0x01, 0};
  expect_string(ident(webp, sizeof webp, 0),
                "RIFF (little-endian) data, Web/P image, extended, 640 x 480, with alpha, animated");
  expect_string(mi.mime, "image/webp");
}

static void check_audio_video(void)
{
  memset(buf, 0, 64);
  memcpy(buf, "RIFF\0\0\0\0WAVEfmt ", 16);
  put32le(buf + 16, 16);
  put16le(buf + 20, 1);
  put16le(buf + 22, 2);
  put32le(buf + 24, 44100);
  put16le(buf + 34, 16);
  expect_string(ident(buf, 44, 0), "RIFF (little-endian) data, WAVE audio, Microsoft PCM, 16 bit, stereo, 44100 Hz");

  static const unsigned char mp3[] = {0xff, 0xfb, 0x90, 0x64};
  expect_string(ident(mp3, 4, 0), "MPEG ADTS, layer III, v1, 44100 Hz, joint stereo");
  static const unsigned char reserved_rate[] = {0xff, 0xfb, 0x9c, 0x64};
  expect(strncmp(ident(reserved_rate, 4, 0), "MPEG", 4) != 0);

  expect_string(ident("\0\0\0\x20"
                      "ftypM4A \0\0\0\0",
                      16, 0),
                "ISO Media, Apple iTunes AAC-LC (.M4A) Audio");
  expect_string(mi.mime, "audio/mp4");
  expect_string(ident("\x1a\x45\xdf\xa3\x9f\x42\x86\x81\x01\x42\x82\x84webm", 16, 0), "WebM");
}

static void check_archives(void)
{
  memset(buf, 0, 64);
  memcpy(buf, "\x1f\x8b\x08\x08\0\0\0\0\0\x03notes.txt", 19);
  put32le(buf + 36, 1234);
  expect_string(ident(buf, 40, 1),
                "gzip compressed data, was \"notes.txt\", from Unix, original size modulo 2^32 1234");
  expect_string(ident(buf, 40, 0), "gzip compressed data, was \"notes.txt\", from Unix");

  memset(buf, 0, 128);
  memcpy(buf, "PK\3\4\x14\0", 6);
  put16le(buf + 26, 5);
  memcpy(buf + 30, "a.txt", 5);
  expect_string(ident(buf, 64, 0), "Zip archive data, at least v2.0 to extract");
  expect_string(mi.mime, "application/zip");

  static const char epub[] = "application/epub+zip";
  put32le(buf + 18, sizeof epub - 1);
  put16le(buf + 26, 8);
  memcpy(buf + 30, "mimetype", 8);
  memcpy(buf + 38, epub, sizeof epub - 1);
  expect_string(ident(buf, 64, 0), "EPUB document");
  expect_string(mi.mime, "application/epub+zip");

  memset(buf, 0, 1024);
  memcpy(buf, "readme.txt", 10);
  memcpy(buf + 257,
         "ustar\0"
         "00",
         8);
  expect_string(ident(buf, 1024, 0), "POSIX tar archive");

  expect_string(ident("\xfd"
                      "7zXZ\0\0\4",
                      8, 0),
                "XZ compressed data, checksum CRC64");
  expect_string(ident("BZh9", 4, 0), "bzip2 compressed data, block size = 900k");
}

static void check_documents(void)
{
  expect_string(ident_str("%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"), "PDF document, version 1.7");
  expect_string(mi.mime, "application/pdf");
  expect_string(ident("SQLite format 3\0", 16, 0), "SQLite 3.x database");

  static const unsigned char ttf[] = {0, 1, 0, 0, 0, 18, 1, 0, 0, 4, 0, 2};
  expect_string(ident(ttf, sizeof ttf, 0), "TrueType Font data, 18 tables");
  expect_string(ident("ttcf\0\1\0\0\0\0\0\x04", 12, 0), "TrueType font collection data, 4 fonts");
}

static void check_disks(void)
{
  memset(buf, 0, sizeof buf);
  memcpy(buf + 0x8001, "CD001", 5);
  memcpy(buf + 0x8028, "OWOS                            ", 32);
  memcpy(buf + 0x8807, "EL TORITO SPECIFICATION", 23);
  buf[510] = 0x55, buf[511] = 0xaa;
  expect_string(ident(buf, sizeof buf, 0), "ISO 9660 CD-ROM filesystem data (DOS/MBR boot sector) \"OWOS\" (bootable)");
  expect_string(ident(buf, 0x8000, 0), "DOS/MBR boot sector");

  memset(buf, 0, 1024);
  buf[510] = 0x55, buf[511] = 0xaa;
  buf[446 + 4] = 0xee;
  memcpy(buf + 512, "EFI PART", 8);
  expect_string(ident(buf, 1024, 0), "DOS/MBR boot sector; GPT partition table");

  memset(buf, 0, 1024);
  buf[510] = 0x55, buf[511] = 0xaa;
  buf[446] = 0x80;
  buf[446 + 4] = 0x0c;
  buf[462 + 4] = 0x83;
  expect_string(ident(buf, 512, 0), "DOS/MBR boot sector, 2 partitions");

  memset(buf, 0, 1024);
  memcpy(buf, "\xeb\x58\x90MSWIN4.1", 11);
  put16le(buf + 11, 512);
  memcpy(buf + 71, "OWOS BOOT  FAT32   ", 19);
  buf[510] = 0x55, buf[511] = 0xaa;
  expect_string(ident(buf, 512, 0), "DOS/MBR boot sector, FAT (32 bit), label: \"OWOS BOOT\"");

  memset(buf, 0, 1024);
  memcpy(buf, "9OWO", 4);
  put32le(buf + 4, 1);
  put32le(buf + 8, 16);
  expect_string(ident(buf, 1024, 0), "OwOS filesystem data, version 9");
  buf[100] = 1;
  expect_string(ident(buf, 1024, 0), "data");

  memset(buf, 0, 64);
  memcpy(buf, "QFI\xfb", 4);
  put32be(buf + 4, 3);
  put32be(buf + 24, 2);
  put32be(buf + 28, 0);
  expect_string(ident(buf, 64, 0), "QEMU QCOW2 Image (v3), 8589934592 bytes");
}

static void check_text(void)
{
  expect_string(ident_str("hello\nworld\n"), "ASCII text");
  expect_string(mi.mime, "text/plain");
  expect_string(mi.charset, "us-ascii");
  expect_string(ident_str("caf\xc3\xa9\n"), "Unicode text, UTF-8 text");
  expect_string(mi.charset, "utf-8");
  expect_string(ident_str("\xef\xbb\xbfhi\n"), "Unicode text, UTF-8 (with BOM) text");
  expect_string(ident_str("caf\xe9\n"), "ISO-8859 text");
  expect_string(mi.charset, "iso-8859-1");
  expect_string(ident_str("a\x85z\n"), "Non-ISO extended-ASCII text");
  expect_string(ident_str("a\r\nb\r\n"), "ASCII text, with CRLF line terminators");
  expect_string(ident_str("a\r\nb\n"), "ASCII text, with CRLF, LF line terminators");
  expect_string(ident_str("no newline"), "ASCII text, with no line terminators");
  expect_string(ident("no newline", 10, 0), "ASCII text");
  expect_string(ident_str("\x1b[1mbold\x1b[0m\n"), "ASCII text, with escape sequences");
  expect_string(ident("a\0b\n", 4, 1), "data");

  /* A multibyte sequence cut off by the read limit is still UTF-8; at the real end it is not. */
  expect_string(ident("ok \xe2\x82", 5, 0), "Unicode text, UTF-8 text");
  expect_string(ident("ok \xe2\x82", 5, 1), "Non-ISO extended-ASCII text, with no line terminators");

  expect_string(ident("\xff\xfeh\0i\0\n\0", 8, 1), "Unicode text, UTF-16, little-endian text");
  expect_string(mi.charset, "utf-16le");
  expect(strncmp(ident("\xff\xfeh\0\1\0", 6, 1), "Unicode", 7) != 0);
}

static void check_text_kinds(void)
{
  expect_string(ident_str("#!/bin/sh\necho hi\n"), "POSIX shell script, ASCII text executable");
  expect_string(mi.mime, "text/x-shellscript");
  expect_string(ident_str("#!/usr/bin/env -S python3 -u\nprint(1)\n"), "Python script, ASCII text executable");
  expect_string(mi.mime, "text/x-script.python");
  expect_string(ident_str("#! /opt/tool/frob\n"), "a frob script, ASCII text executable");
  expect_string(ident_str("<!DOCTYPE html>\n<html></html>\n"), "HTML document, ASCII text");
  expect_string(mi.mime, "text/html");
  expect_string(ident_str("<?xml version=\"1.0\"?>\n<a/>\n"), "XML 1.0 document, ASCII text");
  expect_string(ident_str("<?xml version=\"1.0\"?>\n<svg xmlns=\"x\"/>\n"), "SVG Scalable Vector Graphics image");
  expect_string(mi.mime, "image/svg+xml");
  expect_string(ident_str(" {\"a\": [1, 2]}\n"), "JSON text data");
  expect_string(mi.mime, "application/json");
  expect_string(ident(" {\"a\": [1, 2]}\n", 15, 0), "ASCII text");
  expect_string(ident_str("{ not json\n"), "ASCII text");
  expect_string(ident_str("[Desktop Entry]\nName=Files\n"), "desktop entry, ASCII text");
  expect_string(ident_str("-----BEGIN CERTIFICATE-----\nMIIB\n"), "PEM certificate");
  expect_string(ident_str("-----BEGIN EC PRIVATE KEY-----\nMHc\n"), "PEM private key");
  expect_string(ident_str("ssh-ed25519 AAAAC3Nz user@host\n"), "OpenSSH ED25519 public key");
}

static void check_fallbacks(void)
{
  expect_string(ident("", 0, 1), "empty");
  expect_string(mi.mime, "inode/x-empty");
  static const unsigned char junk[] = {0x00, 0x13, 0x37, 0xfe, 0x01};
  expect_string(ident(junk, sizeof junk, 1), "data");
  expect_string(mi.mime, "application/octet-stream");
  expect_string(mi.charset, "binary");

  /* Truncated headers must not read past the data or leave half a description. */
  expect_string(ident("\177ELF", 4, 0), "data");
  expect_string(ident("\x89PNG\r\n\x1a\n", 8, 0), "PNG image data");
  expect_string(ident("RIFF", 4, 0), "ASCII text");
  for (int n = 0; n <= 40; n++)
    magic_identify("\x1f\x8b\x08\x1c\0\0\0\0\0\x03\xff\xff", n < 12 ? n : 12, 1, &mi);
  expect(strlen(mi.desc) < sizeof mi.desc);
}

int main(void)
{
  test("ELF", check_elf);
  test("PE", check_pe);
  test("images", check_images);
  test("audio and video", check_audio_video);
  test("archives", check_archives);
  test("documents and fonts", check_documents);
  test("disk images", check_disks);
  test("text encodings", check_text);
  test("text kinds", check_text_kinds);
  test("fallbacks", check_fallbacks);
  return 0;
}
