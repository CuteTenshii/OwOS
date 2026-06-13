/* console.c — the kernel console layer (see console.h for the full picture).
 *
 * con_putc() is the one funnel every console write passes through. It fans each
 * byte out to: the serial port, the boot journal (log.c), the framebuffer boot
 * console (fb.c, when verbose), an active capture buffer (file redirect / pipe /
 * the ring-3 terminal bridge), and the VGA text grid below.
 *
 * The VGA text grid is just this file's primary backend: an 80x25 grid at
 * physical 0xB8000, each cell a character + attribute byte, written directly (no
 * BIOS). In graphics mode it becomes a shadow the compositor renders; the
 * VGA width/memory/CRTC constants here are that backend's hardware, kept
 * internal to this file. */

#include <stddef.h>
#include "console.h"
#include "log.h"
#include "fb.h"
#include "serial.h"
#include "io.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile uint16_t *)0xB8000)

/* CRTC index/data ports; registers 14/15 hold the hardware cursor location. */
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

static uint16_t cursor_row;
static uint16_t cursor_col;
static uint8_t  color;
static uint32_t scroll_count;          /* bumped once per row scrolled away   */

/* A shadow copy of the 80x25 grid, always kept current. In text mode it mirrors
 * 0xB8000; in graphics mode it is the source of truth the window manager renders
 * the terminal window from. `gfx` flips when the framebuffer console takes over;
 * `dirty` ticks on every change so the compositor knows when to repaint. */
static uint16_t shadow[VGA_WIDTH * VGA_HEIGHT];
static int      gfx;
static uint32_t dirty;

/* Write one cell to the shadow grid and, while in text mode, straight to VGA
 * memory. Every on-screen character change funnels through here. */
static void cell_write(int idx, uint16_t entry)
{
	shadow[idx] = entry;
	if (!gfx)
		VGA_MEMORY[idx] = entry;
	dirty++;
}

/* When non-NULL, con_putc diverts output here instead of to the screen. */
static char    *capture_buf;
static uint32_t capture_cap;
static uint32_t capture_len;
/* In ANSI capture mode, con_set_color also emits an SGR escape into the capture
 * buffer, so a consumer (the ring-3 terminal's bridge) can reproduce colors.
 * Off for plain captures (file redirect, grep pipe), which want clean text. */
static int      capture_ansi;

/* Append a NUL-terminated string to the capture buffer (bounded). */
static void capture_puts(const char *s)
{
	while (*s && capture_len < capture_cap)
		capture_buf[capture_len++] = *s++;
}

/* Emit an ANSI SGR escape for a VGA attribute byte (fg | bg<<4). VGA's palette
 * order differs from ANSI's, so remap; bit 3 is the bright/bold variant. */
static void capture_emit_color(uint8_t attr)
{
	static const int v2a[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };  /* VGA index -> ANSI */
	int fg = attr & 0x0F, bg = (attr >> 4) & 0x0F;
	int fc = (fg & 8 ? 90 : 30) + v2a[fg & 7];
	int bc = (bg & 8 ? 100 : 40) + v2a[bg & 7];
	char e[24];
	int i = 0;
	e[i++] = 0x1b; e[i++] = '[';
	if (fc >= 100) e[i++] = (char)('0' + fc / 100);
	e[i++] = (char)('0' + fc / 10 % 10);
	e[i++] = (char)('0' + fc % 10);
	e[i++] = ';';
	if (bc >= 100) e[i++] = (char)('0' + bc / 100);
	e[i++] = (char)('0' + bc / 10 % 10);
	e[i++] = (char)('0' + bc % 10);
	e[i++] = 'm';
	e[i] = '\0';
	capture_puts(e);
}

/* Pack a character and the current color into a VGA cell. Mask to a byte first
 * so high-bit CP437 glyphs (é, è, °, ...) don't sign-extend into the color. */
static uint16_t con_entry(char c)
{
	return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

/* Move the blinking hardware cursor to the current row/col so it tracks the
 * text. Without this the cursor stays frozen where the BIOS left it. */
static void con_update_cursor(void)
{
	uint16_t pos = cursor_row * VGA_WIDTH + cursor_col;

	outb(VGA_CRTC_INDEX, 0x0F);
	outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
	outb(VGA_CRTC_INDEX, 0x0E);
	outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

void con_set_color(enum con_color fg, enum con_color bg)
{
	color = (uint8_t)fg | ((uint8_t)bg << 4);
	if (capture_buf && capture_ansi)
		capture_emit_color(color);
}

void con_clear(void)
{
	cursor_row = 0;
	cursor_col = 0;
	for (uint16_t y = 0; y < VGA_HEIGHT; y++)
		for (uint16_t x = 0; x < VGA_WIDTH; x++)
			cell_write(y * VGA_WIDTH + x, con_entry(' '));
	con_update_cursor();
}

void con_init(void)
{
	con_set_color(CON_LIGHT_GREY, CON_BLACK);
	con_clear();
}

/* Scroll the whole screen up by one line and clear the new bottom row. */
static void con_scroll(void)
{
	for (uint16_t y = 1; y < VGA_HEIGHT; y++)
		for (uint16_t x = 0; x < VGA_WIDTH; x++)
			shadow[(y - 1) * VGA_WIDTH + x] = shadow[y * VGA_WIDTH + x];

	for (uint16_t x = 0; x < VGA_WIDTH; x++)
		shadow[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = con_entry(' ');

	if (!gfx)
		for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
			VGA_MEMORY[i] = shadow[i];

	scroll_count++;
	dirty++;
}

/* Number of rows the console has scrolled so far. A caller holding a saved
 * cursor position can watch this across a drawing operation: each increment
 * means every on-screen cell moved up one row (VGA_WIDTH cells). */
uint32_t con_scroll_count(void)
{
	return scroll_count;
}

void con_capture_begin(char *buf, uint32_t cap)
{
	capture_buf = buf;
	capture_cap = cap;
	capture_len = 0;
	capture_ansi = 0;
}

/* Like con_capture_begin, but also records colors as ANSI SGR escapes (for the
 * ring-3 terminal bridge, which renders them). */
void con_capture_begin_ansi(char *buf, uint32_t cap)
{
	con_capture_begin(buf, cap);
	capture_ansi = 1;
}

uint32_t con_capture_end(void)
{
	uint32_t n = capture_len;
	capture_buf = NULL;
	return n;
}

void con_putc(char c)
{
	if (capture_buf) {             /* redirected: collect, don't draw         */
		if (capture_len < capture_cap)
			capture_buf[capture_len++] = c;
		return;
	}

	serial_putc(c);                /* mirror the console to COM1 (the log port) */

	/* During bring-up the console doubles as the kernel log. This funnel is the
	 * single point every console write passes through, so record each character
	 * into the journal here -- no matter which helper produced it (con_write,
	 * con_write_dec/hex, or a subsystem's own logger like e1000's dlog). Unless
	 * the `verbose` boot arg asked for a live boot log, stop without drawing, so
	 * the boot stays quiet on screen and the terminal opens clean. Once the
	 * shell takes the console, capture is off and this is skipped entirely. */
	if (journal_capturing()) {
		journal_record(c);
		if (!log_verbose())
			return;
	}

	/* Verbose boot in graphics mode: the 0xB8000 text below isn't shown and the
	 * compositor isn't up yet, so mirror the log onto a framebuffer console. Only
	 * before the WM takes the framebuffer (!gfx); after that vga draws the shadow
	 * grid the compositor renders. */
	if (log_verbose() && !gfx && fb_active())
		fb_con_putc(c);

	if (c == '\n') {
		cursor_col = 0;
		cursor_row++;
	} else if (c == '\t') {
		/* Expand to spaces up to the next 8-column tab stop. Without this the
		 * raw 0x09 byte would be drawn as its CP437 glyph (a circle). */
		do {
			cell_write(cursor_row * VGA_WIDTH + cursor_col, con_entry(' '));
			if (++cursor_col == VGA_WIDTH) {
				cursor_col = 0;
				cursor_row++;
				break;
			}
		} while (cursor_col % 8 != 0);
	} else if (c == '\b') {
		if (cursor_col > 0) {
			cursor_col--;
		} else if (cursor_row > 0) {
			cursor_row--;
			cursor_col = VGA_WIDTH - 1;
		}
		cell_write(cursor_row * VGA_WIDTH + cursor_col, con_entry(' '));
	} else {
		cell_write(cursor_row * VGA_WIDTH + cursor_col, con_entry(c));
		if (++cursor_col == VGA_WIDTH) {
			cursor_col = 0;
			cursor_row++;
		}
	}

	if (cursor_row == VGA_HEIGHT) {
		con_scroll();
		cursor_row = VGA_HEIGHT - 1;
	}

	con_update_cursor();
}

void con_write(const char *s)
{
	for (size_t i = 0; s[i] != '\0'; i++)
		con_putc(s[i]);
}

/* Linear cursor position (row * width + col), and a setter that just moves the
 * cursor without drawing. The shell's line editor uses these to reposition
 * within a line for left/right movement and in-place redraws. */
uint16_t con_get_pos(void)
{
	return (uint16_t)(cursor_row * VGA_WIDTH + cursor_col);
}

void con_set_pos(uint16_t pos)
{
	if (pos >= VGA_WIDTH * VGA_HEIGHT)
		pos = VGA_WIDTH * VGA_HEIGHT - 1;
	cursor_row = pos / VGA_WIDTH;
	cursor_col = pos % VGA_WIDTH;
	con_update_cursor();
}

/* Slide a saved linear position up by `rows` screen rows (clamping at 0), to
 * follow content that has scrolled. Keeps the width arithmetic in one place. */
uint16_t con_pos_scroll_up(uint16_t pos, uint32_t rows)
{
	uint32_t up = rows * VGA_WIDTH;
	return (up <= pos) ? (uint16_t)(pos - up) : 0;
}

/* --- framebuffer-console hooks (used by the window manager) ---------------- */

/* Switch the console backend. Once on, character writes stop touching 0xB8000
 * and only update the shadow grid; the compositor draws that grid into the
 * terminal window. The boot log is still in `shadow` at this point, but the
 * shell clears it (shell_init) so the terminal opens clean -- the log lives on
 * in the journal (the `journal` command). */
void con_use_framebuffer(int on)
{
	gfx = on ? 1 : 0;
	dirty++;
}

uint32_t        con_dirty(void)      { return dirty; }
const uint16_t *con_shadow(void)     { return shadow; }
int             con_cols(void)       { return VGA_WIDTH; }
int             con_rows(void)       { return VGA_HEIGHT; }

void con_write_hex(uint32_t value)
{
	static const char digits[] = "0123456789ABCDEF";
	con_write("0x");
	for (int shift = 28; shift >= 0; shift -= 4)
		con_putc(digits[(value >> shift) & 0xF]);
}

void con_write_dec(uint32_t value)
{
	char tmp[10];                  /* 2^32 fits in 10 digits                  */
	int i = 0;

	if (value == 0) {
		con_putc('0');
		return;
	}
	while (value) {
		tmp[i++] = '0' + (value % 10);
		value /= 10;
	}
	while (i--)
		con_putc(tmp[i]);          /* digits came out reversed                */
}
