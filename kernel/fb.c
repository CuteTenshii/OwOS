/* fb.c — linear framebuffer driver over the Bochs/QEMU VBE DISPI interface.
 *
 * QEMU's default "std" VGA (and Bochs/VirtualBox) expose a tiny register bank,
 * the "VBE DISPI" interface, that sets a linear-framebuffer graphics mode from
 * protected mode with no real-mode BIOS call. We pick a 32-bpp mode, find the
 * card's linear framebuffer through its PCI BAR0, map it, and draw straight
 * into it. If no DISPI adapter answers, fb_init returns -1 and the kernel keeps
 * its VGA text console.
 */

#include <stddef.h>
#include "fb.h"
#include "io.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "font8x16.h"

/* DISPI index/data ports and register indices. */
#define VBE_DISPI_INDEX   0x01CE
#define VBE_DISPI_DATA    0x01CF
#define VBE_DISPI_ID      0x0
#define VBE_DISPI_XRES    0x1
#define VBE_DISPI_YRES    0x2
#define VBE_DISPI_BPP     0x3
#define VBE_DISPI_ENABLE  0x4
#define VBE_DISPI_VWIDTH  0x6
#define VBE_DISPI_VHEIGHT 0x7
#define VBE_DISPI_XOFF    0x8
#define VBE_DISPI_YOFF    0x9

#define VBE_ENABLED       0x01
#define VBE_LFB_ENABLED   0x40

static uint32_t *front;       /* the visible linear framebuffer (XRGB8888)    */
static uint32_t *fb;          /* off-screen back buffer; primitives draw here */
static uint32_t  fb_w, fb_h;
static uint32_t  fb_pitch;    /* pixels per row                                */
static uint32_t  fb_pixels;   /* total pixels in the mapped region (pitch*h)    */
static int       active;
static int       single;      /* 1 when there is no back buffer (draw direct)   */

/* Clip rectangle: drawing primitives only touch pixels inside it, so the
 * compositor can redraw just a damaged region by clipping to it. Half-open
 * [cl_x0,cl_x1) x [cl_y0,cl_y1). */
static int cl_x0, cl_y0, cl_x1, cl_y1;

/* Damage rectangle: the bounding box of everything drawn since the last
 * present. fb_present copies only this region from the back buffer to VRAM —
 * the key to staying fast on real hardware, where VRAM writes are slow. Empty
 * when dirty_x1 <= dirty_x0. */
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;

void fb_clip_full(void)
{
	cl_x0 = 0; cl_y0 = 0; cl_x1 = (int)fb_w; cl_y1 = (int)fb_h;
}

void fb_set_clip(int x, int y, int w, int h)
{
	cl_x0 = x < 0 ? 0 : x;
	cl_y0 = y < 0 ? 0 : y;
	cl_x1 = (x + w > (int)fb_w) ? (int)fb_w : x + w;
	cl_y1 = (y + h > (int)fb_h) ? (int)fb_h : y + h;
	if (cl_x1 < cl_x0) cl_x1 = cl_x0;
	if (cl_y1 < cl_y0) cl_y1 = cl_y0;
}

/* Intersect (x,y,w,h) with the clip rect into out[], returns 0 if empty. */
static int clip(int x, int y, int w, int h, int *ox, int *oy, int *ow, int *oh)
{
	int x0 = x < cl_x0 ? cl_x0 : x;
	int y0 = y < cl_y0 ? cl_y0 : y;
	int x1 = x + w; if (x1 > cl_x1) x1 = cl_x1;
	int y1 = y + h; if (y1 > cl_y1) y1 = cl_y1;
	if (x0 >= x1 || y0 >= y1)
		return 0;
	*ox = x0; *oy = y0; *ow = x1 - x0; *oh = y1 - y0;
	return 1;
}

/* Grow the damage box to include the (already clipped) rectangle. */
static void mark(int x, int y, int w, int h)
{
	if (dirty_x1 <= dirty_x0) {            /* was empty                        */
		dirty_x0 = x; dirty_y0 = y; dirty_x1 = x + w; dirty_y1 = y + h;
		return;
	}
	if (x < dirty_x0)         dirty_x0 = x;
	if (y < dirty_y0)         dirty_y0 = y;
	if (x + w > dirty_x1)     dirty_x1 = x + w;
	if (y + h > dirty_y1)     dirty_y1 = y + h;
}

static void alloc_backbuffer(void);

/* Make the visible framebuffer write-combining. On real GPUs VRAM defaults to
 * uncached, so each store is its own slow bus transaction; write-combining lets
 * the CPU batch them into bursts — the single biggest win for the per-frame
 * copy. We program PAT entry 1 to WC and tag the framebuffer's PTEs to use it.
 * A no-op (harmless) under QEMU, whose "VRAM" is host RAM. */
static void enable_wc(void)
{
	uint32_t a, b, c, d;
	__asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	                          : "a"(1), "c"(0));
	if (!(d & (1u << 16)))         /* CPUID.01H:EDX.PAT — no PAT, give up      */
		return;

	/* Read IA32_PAT (MSR 0x277), set entry 1 (PA1) to WC (0x01), write it back.
	 * Editing only PA1 preserves whatever the firmware set for the rest. */
	uint32_t lo, hi;
	__asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
	lo = (lo & ~0x0000FF00u) | (0x01u << 8);
	__asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(0x277));

	/* Drop any cached lines for the framebuffer, then switch it to WC. */
	__asm__ volatile ("wbinvd" ::: "memory");
	paging_mark_wc((uint32_t)front, fb_pixels * 4);
}

static void dispi_write(uint16_t index, uint16_t value)
{
	outw(VBE_DISPI_INDEX, index);
	outw(VBE_DISPI_DATA, value);
}

static uint16_t dispi_read(uint16_t index)
{
	outw(VBE_DISPI_INDEX, index);
	return inw(VBE_DISPI_DATA);
}

/* Find the linear framebuffer's physical base: the first PCI display
 * controller's BAR0 (a prefetchable memory BAR on stdvga). Returns 0 if none. */
static uint32_t find_lfb_phys(void)
{
	for (int bus = 0; bus < 256; bus++) {
		for (int slot = 0; slot < 32; slot++) {
			uint32_t id = pci_read32(bus, slot, 0, 0x00);
			if ((id & 0xFFFF) == 0xFFFF)
				continue;
			uint32_t cls = pci_read32(bus, slot, 0, 0x08);
			if (((cls >> 24) & 0xFF) != 0x03)        /* display controller   */
				continue;
			uint32_t bar0 = pci_read32(bus, slot, 0, 0x10);
			if (bar0 & 0x1)                          /* must be a memory BAR  */
				continue;
			/* Make sure the card decodes memory space and can master. */
			uint32_t cmd = pci_read32(bus, slot, 0, 0x04);
			pci_write32(bus, slot, 0, 0x04, cmd | 0x6);
			return bar0 & 0xFFFFFFF0u;
		}
	}
	return 0;
}

/* Adopt a framebuffer a bootloader (GRUB via Multiboot, or UEFI GOP) already
 * set up for us, instead of programming a mode ourselves. `pitch` is in bytes
 * and may exceed width*4. We only handle 32-bpp direct-RGB framebuffers, which
 * is what our XRGB8888 primitives assume. Returns 0 on success, -1 otherwise. */
int fb_adopt(uint32_t phys, uint32_t w, uint32_t h, uint32_t pitch_bytes, uint32_t bpp)
{
	if (bpp != 32 || phys == 0 || w == 0 || h == 0)
		return -1;
	if (pitch_bytes & 0x3)                         /* must be 4-byte aligned    */
		return -1;

	uint32_t pitch_px = pitch_bytes / 4;
	uint32_t pixels   = pitch_px * h;

	/* Map the whole visible region (full pitch, all rows) into the address
	 * space. The framebuffer may live high in the physical map on real
	 * hardware; paging_identity_map allocates page tables as needed. */
	if (paging_identity_map(phys, pixels * 4) < 0)
		return -1;
	front = (uint32_t *)phys;

	fb_w = w;
	fb_h = h;
	fb_pitch = pitch_px;
	fb_pixels = pixels;

	enable_wc();               /* write-combining on the framebuffer          */
	alloc_backbuffer();        /* off-screen buffer in cached RAM              */

	active = 1;
	fb_clip_full();
	fb_fill_rect(0, 0, (int)fb_w, (int)fb_h, 0x000000);
	fb_present();
	return 0;
}

/* Allocate the off-screen back buffer in cached system RAM. VRAM is the wrong
 * place for it: on real GPUs VRAM is uncached/write-combining, so reading it
 * back (and even drawing into it) is glacially slow. We pull a contiguous run
 * from the PMM (above the 16 MiB identity map — there's plenty on real
 * machines) and identity-map it. Falls back to single-buffered (draw straight
 * to the framebuffer) only if no RAM is available. */
static void alloc_backbuffer(void)
{
	uint32_t bytes = fb_pixels * 4;
	uint32_t bb = pmm_alloc_contig(bytes);
	if (bb && paging_identity_map(bb, bytes) == 0) {
		fb = (uint32_t *)bb;
		single = 0;
	} else {
		fb = front;            /* last resort: draw directly to VRAM           */
		single = 1;
	}
}

int fb_init(uint32_t want_w, uint32_t want_h)
{
	/* The DISPI ID register reads back 0xB0Cx on a capable adapter. */
	uint16_t magic = dispi_read(VBE_DISPI_ID);
	if ((magic & 0xFFF0) != 0xB0C0)
		return -1;

	uint32_t lfb = find_lfb_phys();
	if (!lfb)
		return -1;

	/* Program the mode: disable, set geometry, re-enable with the LFB. */
	dispi_write(VBE_DISPI_ENABLE, 0);
	dispi_write(VBE_DISPI_XRES, (uint16_t)want_w);
	dispi_write(VBE_DISPI_YRES, (uint16_t)want_h);
	dispi_write(VBE_DISPI_BPP, 32);
	dispi_write(VBE_DISPI_VWIDTH, (uint16_t)want_w);
	dispi_write(VBE_DISPI_VHEIGHT, (uint16_t)want_h);
	dispi_write(VBE_DISPI_XOFF, 0);
	dispi_write(VBE_DISPI_YOFF, 0);
	dispi_write(VBE_DISPI_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);

	/* Confirm the adapter accepted the geometry we asked for. */
	if (dispi_read(VBE_DISPI_XRES) != (uint16_t)want_w ||
	    dispi_read(VBE_DISPI_YRES) != (uint16_t)want_h) {
		dispi_write(VBE_DISPI_ENABLE, 0);
		return -1;
	}

	fb_w = want_w;
	fb_h = want_h;
	fb_pitch = want_w;
	fb_pixels = want_w * want_h;

	/* Map the visible framebuffer (one copy), then a RAM back buffer. */
	if (paging_identity_map(lfb, fb_pixels * 4) < 0)
		return -1;
	front = (uint32_t *)lfb;
	enable_wc();
	alloc_backbuffer();

	active = 1;
	fb_clip_full();
	fb_fill_rect(0, 0, (int)fb_w, (int)fb_h, 0x000000);
	fb_present();
	return 0;
}

void fb_present(void)
{
	if (single) {                  /* drawing went straight to the framebuffer  */
		dirty_x1 = dirty_x0;       /* nothing to copy; just clear the damage    */
		return;
	}
	if (dirty_x1 <= dirty_x0 || dirty_y1 <= dirty_y0)
		return;                    /* nothing changed                           */

	/* Copy only the damaged rectangle, row by row, from the cached RAM back
	 * buffer to VRAM. This is the only place we ever write the framebuffer. */
	for (int y = dirty_y0; y < dirty_y1; y++) {
		const uint32_t *s = fb    + (uint32_t)y * fb_pitch + dirty_x0;
		uint32_t       *d = front + (uint32_t)y * fb_pitch + dirty_x0;
		for (int x = 0; x < dirty_x1 - dirty_x0; x++)
			d[x] = s[x];
	}
	/* Drain the write-combining buffer to VRAM. The front buffer is WC
	 * (enable_wc), so these stores otherwise linger in the CPU's WC buffer and
	 * don't reach the panel until some later eviction — which is why a small
	 * present (clock tick, cursor move) stayed invisible on real hardware until
	 * a burst of input forced a flush. SFENCE makes the frame appear now. */
	__asm__ volatile ("sfence" ::: "memory");
	dirty_x1 = dirty_x0;           /* mark the damage box empty again           */
}

int      fb_active(void) { return active; }
uint32_t fb_width(void)  { return fb_w; }
uint32_t fb_height(void) { return fb_h; }
uint32_t *fb_ptr(void)   { return fb; }
uint32_t fb_pitch_pixels(void) { return fb_pitch; }

void fb_pixel(int x, int y, uint32_t color)
{
	if (x < cl_x0 || x >= cl_x1 || y < cl_y0 || y >= cl_y1)
		return;
	fb[y * (int)fb_pitch + x] = color;
	mark(x, y, 1, 1);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
	int cx, cy, cw, ch;
	if (!clip(x, y, w, h, &cx, &cy, &cw, &ch))
		return;
	for (int row = 0; row < ch; row++) {
		uint32_t *p = fb + (cy + row) * (int)fb_pitch + cx;
		for (int col = 0; col < cw; col++)
			p[col] = color;
	}
	mark(cx, cy, cw, ch);
}

void fb_rect(int x, int y, int w, int h, uint32_t color)
{
	fb_fill_rect(x, y, w, 1, color);
	fb_fill_rect(x, y + h - 1, w, 1, color);
	fb_fill_rect(x, y, 1, h, color);
	fb_fill_rect(x + w - 1, y, 1, h, color);
}

void fb_blit(int x, int y, int w, int h, const uint32_t *src, int src_pitch)
{
	int cx, cy, cw, ch;
	if (!clip(x, y, w, h, &cx, &cy, &cw, &ch))
		return;
	for (int row = 0; row < ch; row++) {
		const uint32_t *s = src + (cy - y + row) * src_pitch + (cx - x);
		uint32_t *d = fb + (cy + row) * (int)fb_pitch + cx;
		for (int col = 0; col < cw; col++)
			d[col] = s[col];
	}
	mark(cx, cy, cw, ch);
}

void fb_glyph(int x, int y, unsigned char ch, uint32_t fg, uint32_t bg)
{
	int cx, cy, cw, chh;
	if (!clip(x, y, 8, 16, &cx, &cy, &cw, &chh))
		return;
	const unsigned char *g = font8x16[ch];
	int transparent = (bg & FB_TRANSPARENT) != 0;
	for (int row = 0; row < chh; row++) {
		int sy = cy + row;
		unsigned char bits = g[sy - y];
		uint32_t *d = fb + sy * (int)fb_pitch;
		for (int col = 0; col < cw; col++) {
			int sx = cx + col;
			if (bits & (0x80 >> (sx - x)))
				d[sx] = fg;
			else if (!transparent)
				d[sx] = bg;
		}
	}
	mark(cx, cy, cw, chh);
}

void fb_text(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
	for (; *s; s++, x += 8)
		fb_glyph(x, y, (unsigned char)*s, fg, bg);
}
