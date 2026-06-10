/* vga.c — VGA text-mode console driver.
 *
 * Text mode maps an 80x25 grid to physical memory at 0xB8000. Each cell is
 * two bytes: an ASCII character and an attribute byte (foreground/background
 * color). Writing there puts characters directly on screen — no BIOS calls.
 */

#include <stddef.h>
#include "vga.h"
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

/* When non-NULL, vga_putc diverts output here instead of to the screen. */
static char    *capture_buf;
static uint32_t capture_cap;
static uint32_t capture_len;

/* Pack a character and the current color into a VGA cell. Mask to a byte first
 * so high-bit CP437 glyphs (é, è, °, ...) don't sign-extend into the color. */
static uint16_t vga_entry(char c)
{
	return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

/* Move the blinking hardware cursor to the current row/col so it tracks the
 * text. Without this the cursor stays frozen where the BIOS left it. */
static void vga_update_cursor(void)
{
	uint16_t pos = cursor_row * VGA_WIDTH + cursor_col;

	outb(VGA_CRTC_INDEX, 0x0F);
	outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
	outb(VGA_CRTC_INDEX, 0x0E);
	outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
	color = (uint8_t)fg | ((uint8_t)bg << 4);
}

void vga_clear(void)
{
	cursor_row = 0;
	cursor_col = 0;
	for (uint16_t y = 0; y < VGA_HEIGHT; y++)
		for (uint16_t x = 0; x < VGA_WIDTH; x++)
			cell_write(y * VGA_WIDTH + x, vga_entry(' '));
	vga_update_cursor();
}

void vga_init(void)
{
	vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
	vga_clear();
}

/* Scroll the whole screen up by one line and clear the new bottom row. */
static void vga_scroll(void)
{
	for (uint16_t y = 1; y < VGA_HEIGHT; y++)
		for (uint16_t x = 0; x < VGA_WIDTH; x++)
			shadow[(y - 1) * VGA_WIDTH + x] = shadow[y * VGA_WIDTH + x];

	for (uint16_t x = 0; x < VGA_WIDTH; x++)
		shadow[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ');

	if (!gfx)
		for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
			VGA_MEMORY[i] = shadow[i];

	scroll_count++;
	dirty++;
}

/* Number of rows the console has scrolled so far. A caller holding a saved
 * cursor position can watch this across a drawing operation: each increment
 * means every on-screen cell moved up one row (VGA_WIDTH cells). */
uint32_t vga_scroll_count(void)
{
	return scroll_count;
}

void vga_capture_begin(char *buf, uint32_t cap)
{
	capture_buf = buf;
	capture_cap = cap;
	capture_len = 0;
}

uint32_t vga_capture_end(void)
{
	uint32_t n = capture_len;
	capture_buf = NULL;
	return n;
}

void vga_putc(char c)
{
	if (capture_buf) {             /* redirected: collect, don't draw         */
		if (capture_len < capture_cap)
			capture_buf[capture_len++] = c;
		return;
	}

	serial_putc(c);                /* mirror the console to COM1 for logging  */

	if (c == '\n') {
		cursor_col = 0;
		cursor_row++;
	} else if (c == '\t') {
		/* Expand to spaces up to the next 8-column tab stop. Without this the
		 * raw 0x09 byte would be drawn as its CP437 glyph (a circle). */
		do {
			cell_write(cursor_row * VGA_WIDTH + cursor_col, vga_entry(' '));
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
		cell_write(cursor_row * VGA_WIDTH + cursor_col, vga_entry(' '));
	} else {
		cell_write(cursor_row * VGA_WIDTH + cursor_col, vga_entry(c));
		if (++cursor_col == VGA_WIDTH) {
			cursor_col = 0;
			cursor_row++;
		}
	}

	if (cursor_row == VGA_HEIGHT) {
		vga_scroll();
		cursor_row = VGA_HEIGHT - 1;
	}

	vga_update_cursor();
}

void vga_write(const char *s)
{
	for (size_t i = 0; s[i] != '\0'; i++)
		vga_putc(s[i]);
}

/* Linear cursor position (row * width + col), and a setter that just moves the
 * cursor without drawing. The shell's line editor uses these to reposition
 * within a line for left/right movement and in-place redraws. */
uint16_t vga_get_pos(void)
{
	return (uint16_t)(cursor_row * VGA_WIDTH + cursor_col);
}

void vga_set_pos(uint16_t pos)
{
	if (pos >= VGA_WIDTH * VGA_HEIGHT)
		pos = VGA_WIDTH * VGA_HEIGHT - 1;
	cursor_row = pos / VGA_WIDTH;
	cursor_col = pos % VGA_WIDTH;
	vga_update_cursor();
}

/* Slide a saved linear position up by `rows` screen rows (clamping at 0), to
 * follow content that has scrolled. Keeps the width arithmetic in one place. */
uint16_t vga_pos_scroll_up(uint16_t pos, uint32_t rows)
{
	uint32_t up = rows * VGA_WIDTH;
	return (up <= pos) ? (uint16_t)(pos - up) : 0;
}

/* --- framebuffer-console hooks (used by the window manager) ---------------- */

/* Switch the console backend. Once on, character writes stop touching 0xB8000
 * and only update the shadow grid; the compositor draws that grid into the
 * terminal window. The current screen contents are already in `shadow`, so the
 * terminal window comes up showing the boot log. */
void vga_use_framebuffer(int on)
{
	gfx = on ? 1 : 0;
	dirty++;
}

uint32_t        vga_dirty(void)      { return dirty; }
const uint16_t *vga_shadow(void)     { return shadow; }
int             vga_cols(void)       { return VGA_WIDTH; }
int             vga_rows(void)       { return VGA_HEIGHT; }

void vga_write_hex(uint32_t value)
{
	static const char digits[] = "0123456789ABCDEF";
	vga_write("0x");
	for (int shift = 28; shift >= 0; shift -= 4)
		vga_putc(digits[(value >> shift) & 0xF]);
}

void vga_write_dec(uint32_t value)
{
	char tmp[10];                  /* 2^32 fits in 10 digits                  */
	int i = 0;

	if (value == 0) {
		vga_putc('0');
		return;
	}
	while (value) {
		tmp[i++] = '0' + (value % 10);
		value /= 10;
	}
	while (i--)
		vga_putc(tmp[i]);          /* digits came out reversed                */
}
