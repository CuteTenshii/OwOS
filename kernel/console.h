/* console.h — the kernel console layer (formerly "vga").
 *
 * con_putc() is the single funnel every console write passes through. It is NOT
 * a hardware driver; it fans the byte out to several sinks:
 *
 *   con_putc(c) ─┬─▶ serial_putc(c)        COM1 (the debug/log port, always)
 *                ├─▶ journal_record(c)     the boot-log ring buffer (log.c)
 *                ├─▶ fb_con_putc(c)        framebuffer boot console (fb.c, verbose)
 *                ├─▶ the 80x25 text grid   0xB8000 in text mode; a shadow the WM
 *                │                          renders in graphics mode
 *                └─▶ a capture buffer       when one is active (file redirect,
 *                                           a "| grep" pipe, or the terminal
 *                                           bridge — which also records colors)
 *
 * So "the console" is this multiplexer; the VGA text grid (0xB8000, the CRTC
 * cursor) is just one backend, kept internal to console.c. Subsystems log by
 * calling con_write/con_set_color; they neither know nor care which sinks are
 * live. The terminal's bridge reuses every kernel builtin by switching
 * the capture sink on and reading back what they printed. */
#ifndef OWOS_CONSOLE_H
#define OWOS_CONSOLE_H

#include <stdint.h>

/* The 16-colour text palette (CGA/VGA order). Backends map it to RGB (fb) or a
 * VGA attribute byte (text grid); the capture sink encodes it as ANSI. */
enum con_color {
	CON_BLACK = 0, CON_BLUE, CON_GREEN, CON_CYAN,
	CON_RED, CON_MAGENTA, CON_BROWN, CON_LIGHT_GREY,
	CON_DARK_GREY, CON_LIGHT_BLUE, CON_LIGHT_GREEN, CON_LIGHT_CYAN,
	CON_LIGHT_RED, CON_LIGHT_MAGENTA, CON_LIGHT_BROWN, CON_WHITE,
};

void con_init(void);
void con_clear(void);
void con_set_color(enum con_color fg, enum con_color bg);
void con_putc(char c);
void con_write(const char *s);
void con_printf(const char *fmt, ...);  /* formatted write; see kstring.h kvformat */
void con_write_hex(uint32_t value);   /* prints "0x" + 8 hex digits */
void con_write_dec(uint32_t value);   /* prints unsigned decimal     */
void con_put_hex(uint32_t value, int digits);  /* `digits` lowercase hex, no "0x" */

/* Hardware-cursor position as a linear offset (row * width + col). con_set_pos
 * moves the cursor only -- it draws nothing -- so a caller can reposition
 * within already-drawn text (used by the shell's line editor). */
uint16_t con_get_pos(void);
void     con_set_pos(uint16_t pos);

/* Monotonic count of rows scrolled off the top. Compare it before and after a
 * sequence of con_putc calls to learn how far a saved position has shifted up. */
uint32_t con_scroll_count(void);

/* Move a saved linear position up by `rows` rows (clamped at 0), to keep an
 * anchor pointing at the same text after the console has scrolled. */
uint16_t con_pos_scroll_up(uint16_t pos, uint32_t rows);

/* Output redirection: while a capture is active, con_putc appends characters
 * to buf (up to cap bytes) instead of drawing them, so a command's text can be
 * sent to a file. con_capture_end stops capturing and returns the byte count. */
void     con_capture_begin(char *buf, uint32_t cap);
void     con_capture_begin_ansi(char *buf, uint32_t cap);  /* + colors as ANSI SGR */
void     con_capture_begin_sink(void (*sink)(char));       /* stream bytes + ANSI SGR */
uint32_t con_capture_end(void);

/* Framebuffer-console hooks. The window manager flips the console into
 * graphics mode and renders the shadow grid into the terminal window. */
void            con_use_framebuffer(int on);
uint32_t        con_dirty(void);     /* ticks on every cell change             */
const uint16_t *con_shadow(void);    /* the 80x25 grid (char | attr<<8)        */
int             con_cols(void);
int             con_rows(void);

#endif /* OWOS_CONSOLE_H */
