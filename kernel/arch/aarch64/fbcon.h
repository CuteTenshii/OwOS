/* fbcon.h: text console on the linear framebuffer the bootloader left running. */
#ifndef OWOS_ARCH_AARCH64_FBCON_H
#define OWOS_ARCH_AARCH64_FBCON_H

#include <stdint.h>

enum fb_format { FB_NONE, FB_XRGB8888, FB_XBGR8888, FB_RGB565 };

struct fb_info {
  uintptr_t base;
  uint32_t width, height, stride; /* pixels, pixels, bytes */
  enum fb_format format;
};

/* Take over the framebuffer and clear it. Returns -1 for a layout it cannot draw on. */
int fbcon_init(const struct fb_info *fb);
void fbcon_putc(char c);

/* Format name from a simple-framebuffer "format" property; FB_NONE if unsupported. */
enum fb_format fb_format_parse(const char *name);

#endif
