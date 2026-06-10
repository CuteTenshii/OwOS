/* vga.h — minimal VGA text-mode console (80x25, memory at 0xB8000). */
#ifndef OWOS_VGA_H
#define OWOS_VGA_H

#include <stdint.h>

/* The 16 colors of the VGA text palette. */
enum vga_color {
	VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN,
	VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LIGHT_GREY,
	VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
	VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_LIGHT_BROWN, VGA_WHITE,
};

void vga_init(void);
void vga_clear(void);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_putc(char c);
void vga_write(const char *s);
void vga_write_hex(uint32_t value);   /* prints "0x" + 8 hex digits */
void vga_write_dec(uint32_t value);   /* prints unsigned decimal     */

/* Hardware-cursor position as a linear offset (row * width + col). vga_set_pos
 * moves the cursor only -- it draws nothing -- so a caller can reposition
 * within already-drawn text (used by the shell's line editor). */
uint16_t vga_get_pos(void);
void     vga_set_pos(uint16_t pos);

/* Monotonic count of rows scrolled off the top. Compare it before and after a
 * sequence of vga_putc calls to learn how far a saved position has shifted up. */
uint32_t vga_scroll_count(void);

/* Move a saved linear position up by `rows` rows (clamped at 0), to keep an
 * anchor pointing at the same text after the console has scrolled. */
uint16_t vga_pos_scroll_up(uint16_t pos, uint32_t rows);

/* Output redirection: while a capture is active, vga_putc appends characters
 * to buf (up to cap bytes) instead of drawing them, so a command's text can be
 * sent to a file. vga_capture_end stops capturing and returns the byte count. */
void     vga_capture_begin(char *buf, uint32_t cap);
uint32_t vga_capture_end(void);

/* Framebuffer-console hooks. The window manager flips the console into
 * graphics mode and renders the shadow grid into the terminal window. */
void            vga_use_framebuffer(int on);
uint32_t        vga_dirty(void);     /* ticks on every cell change             */
const uint16_t *vga_shadow(void);    /* the 80x25 grid (char | attr<<8)        */
int             vga_cols(void);
int             vga_rows(void);

#endif /* OWOS_VGA_H */
