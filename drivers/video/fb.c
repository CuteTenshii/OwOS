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
#include "mtrr.h"
#include "font_aa.h"
#include "font_ui.h"
#include "kstring.h"

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
static uint32_t  fb_pixels;   /* visible pixels in the current mode (pitch*h)   */
static uint32_t  fb_cap;      /* pixels mapped/allocated (>= fb_pixels)         */
static int       active;
static int       single;      /* 1 when there is no back buffer (draw direct)   */
static int       can_mode;    /* 1 = DISPI path: the mode can be reprogrammed   */

/* Resolutions offered for runtime switching (DISPI path only). The front buffer
 * and back buffer are mapped/allocated at the largest of these once at init, so
 * a mode change is just a DISPI reprogram — no remap, no realloc, no race with
 * the compositor. Keep the last entry the largest (it sizes the capacity). */
static const struct { uint16_t w, h; } fb_modes[] = {
	{  800,  600 }, { 1024,  768 }, { 1280,  720 }, { 1280,  800 },
	{ 1280, 1024 }, { 1366,  768 }, { 1440,  900 }, { 1600,  900 },
	{ 1680, 1050 }, { 1920, 1080 },
};
#define FB_NMODES ((int)(sizeof fb_modes / sizeof fb_modes[0]))
#define FB_MAX_W  1920u
#define FB_MAX_H  1080u

/* Display brightness, FB_BRIGHT_MIN..FB_BRIGHT_MAX (max = undimmed). Applied as
 * a per-channel scale when the back buffer is copied to the visible screen. */
static int       brightness = FB_BRIGHT_MAX;

/* Compositor frame-pacing target (Hz) — the desktop's software present cadence,
 * NOT a hardware mode. Settings writes it via /sys/class/graphics/fb0/rate and
 * /bin/compositor polls it to pace its loop. The panel's real refresh caps the
 * useful upper end but isn't enforced here (Settings offers the sensible list). */
#define FB_HZ_MIN 20
#define FB_HZ_MAX 240
static int       target_hz = 60;

/* Scale a back-buffer pixel by the current brightness. FB_BRIGHT_MAX is 8, so
 * the divide is a >>3 the compiler can fold; callers skip this entirely at full
 * brightness (the common case) to keep the per-frame copy a plain memory move. */
static inline uint32_t dim_px(uint32_t p)
{
	uint32_t r = (((p >> 16) & 0xFF) * (uint32_t)brightness) >> 3;
	uint32_t g = (((p >>  8) & 0xFF) * (uint32_t)brightness) >> 3;
	uint32_t b = (( p        & 0xFF) * (uint32_t)brightness) >> 3;
	return (r << 16) | (g << 8) | b;
}

/* 32-bpp run fill (`rep stosl`): a solid-colour row is the inner loop of every
 * rect fill, so a scalar per-pixel store at -O1 wasted most of a full-screen
 * clear. memset can't do this (it broadcasts a byte, not a 32-bit pixel). */
static inline void fill32(uint32_t *d, uint32_t v, int n)
{
	int d0, d1;
	__asm__ volatile("rep ; stosl"
		: "=&D"(d0), "=&c"(d1)
		: "a"(v), "0"(d), "1"(n)
		: "memory");
}

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

/* Current (clamped) clip rectangle, so a caller that iterates the whole screen
 * to render — the desktop background — can confine its loop to the damaged band
 * instead of every row at the panel's full height. */
void fb_get_clip(int *x, int *y, int *w, int *h)
{
	if (x) *x = cl_x0;
	if (y) *y = cl_y0;
	if (w) *w = cl_x1 - cl_x0;
	if (h) *h = cl_y1 - cl_y0;
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

/* Optional flush hook for paravirtual adapters (VMware SVGA II). Set, it is
 * called with each rectangle copied to the visible framebuffer so the backend
 * can tell the host to re-read it; unset (plain LFB), presents are just the copy. */
static void (*present_hook)(int, int, int, int);

void fb_set_present_hook(void (*cb)(int, int, int, int))
{
	present_hook = cb;
}

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
	paging_mark_wc((uint32_t)front, fb_cap * 4);

	/* PAT alone isn't enough on real UEFI hardware: a firmware MTRR can mark the
	 * framebuffer UC, which overrides PAT-WC and leaves every store slow. Pin the
	 * range WC with a variable MTRR too (no-op under QEMU / without MTRRs). */
	mtrr_set_wc((uint32_t)front, fb_cap * 4);
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
	fb_cap = pixels;           /* a bootloader framebuffer is fixed-size        */
	can_mode = 0;              /* cannot reprogram a GOP/adopted framebuffer    */

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
	uint32_t bytes = fb_cap * 4;
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

	/* Map and back the LFB at the largest offered mode, not just the initial
	 * one, so a later fb_set_mode is a pure DISPI reprogram with no remap/realloc
	 * (and so no race with the compositor). QEMU's std-vga VRAM aperture (16 MiB)
	 * comfortably holds 1920x1080x4. */
	fb_cap = FB_MAX_W * FB_MAX_H;
	if (fb_cap < fb_pixels) fb_cap = fb_pixels;

	/* Map the visible framebuffer (one copy), then a RAM back buffer. */
	if (paging_identity_map(lfb, fb_cap * 4) < 0)
		return -1;
	front = (uint32_t *)lfb;
	enable_wc();
	alloc_backbuffer();

	active = 1;
	can_mode = 1;              /* DISPI: the mode can be reprogrammed at runtime */
	fb_clip_full();
	fb_fill_rect(0, 0, (int)fb_w, (int)fb_h, 0x000000);
	fb_present();
	return 0;
}

int fb_can_set_mode(void) { return active && can_mode; }
int fb_mode_count(void)   { return FB_NMODES; }

int fb_mode_get(int i, uint32_t *w, uint32_t *h)
{
	if (i < 0 || i >= FB_NMODES) return 0;
	*w = fb_modes[i].w;
	*h = fb_modes[i].h;
	return 1;
}

/* Reprogram the DISPI adapter to one of the offered modes. The buffers are
 * already sized for the largest mode (fb_init), so this only reshapes the
 * geometry the primitives use, clears the screen, and presents. Must run on the
 * compositor thread (it mutates fb_w/fb_h/fb_pitch the WM draws against). */
int fb_set_mode(uint32_t w, uint32_t h)
{
	if (!active || !can_mode)
		return -1;

	int known = 0;
	for (int i = 0; i < FB_NMODES; i++)
		if (fb_modes[i].w == w && fb_modes[i].h == h) { known = 1; break; }
	if (!known || w * h > fb_cap)
		return -1;
	if (w == fb_w && h == fb_h)
		return 0;                              /* already there                  */

	dispi_write(VBE_DISPI_ENABLE, 0);
	dispi_write(VBE_DISPI_XRES, (uint16_t)w);
	dispi_write(VBE_DISPI_YRES, (uint16_t)h);
	dispi_write(VBE_DISPI_BPP, 32);
	dispi_write(VBE_DISPI_VWIDTH, (uint16_t)w);
	dispi_write(VBE_DISPI_VHEIGHT, (uint16_t)h);
	dispi_write(VBE_DISPI_XOFF, 0);
	dispi_write(VBE_DISPI_YOFF, 0);
	dispi_write(VBE_DISPI_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);
	if (dispi_read(VBE_DISPI_XRES) != (uint16_t)w ||
	    dispi_read(VBE_DISPI_YRES) != (uint16_t)h)
		return -1;                             /* adapter rejected the geometry  */

	fb_w = w;
	fb_h = h;
	fb_pitch = w;
	fb_pixels = w * h;
	fb_clip_full();
	fb_fill_rect(0, 0, (int)fb_w, (int)fb_h, 0x000000);
	fb_present();
	return 0;
}

void fb_present(void)
{
	if (dirty_x1 <= dirty_x0 || dirty_y1 <= dirty_y0) {
		dirty_x1 = dirty_x0;       /* nothing changed                           */
		return;
	}

	int rx = dirty_x0, ry = dirty_y0;
	int rw = dirty_x1 - dirty_x0, rh = dirty_y1 - dirty_y0;

	/* Copy only the damaged rectangle, row by row, from the cached RAM back
	 * buffer to VRAM. This is the only place we ever write the framebuffer.
	 * When dimmed, each pixel is scaled on the way out; at full brightness the
	 * copy stays a verbatim move. In single-buffered fallback drawing already
	 * went straight to the framebuffer, so there is nothing to copy. */
	if (!single) {
		int dim = brightness < FB_BRIGHT_MAX;
		for (int y = dirty_y0; y < dirty_y1; y++) {
			const uint32_t *s = fb    + (uint32_t)y * fb_pitch + dirty_x0;
			uint32_t       *d = front + (uint32_t)y * fb_pitch + dirty_x0;
			if (dim) {
				for (int x = 0; x < rw; x++)
					d[x] = dim_px(s[x]);
			} else {
				memcpy(d, s, (uint32_t)rw * 4);
			}
		}
		/* Drain the write-combining buffer to VRAM. The front buffer is WC
		 * (enable_wc), so these stores otherwise linger in the CPU's WC buffer
		 * and don't reach the panel until some later eviction — which is why a
		 * small present (clock tick, cursor move) stayed invisible on real
		 * hardware until a burst of input forced a flush. SFENCE makes the frame
		 * appear now. */
		__asm__ volatile ("sfence" ::: "memory");
	}

	/* Paravirtual adapter (VMware SVGA II): flush just the damaged rect. */
	if (present_hook)
		present_hook(rx, ry, rw, rh);

	dirty_x1 = dirty_x0;           /* mark the damage box empty again           */
}

/* Step the display brightness and re-render the whole visible screen from the
 * (full-colour) back buffer so the change is immediately visible — the WM only
 * presents damaged rectangles, so without this a brightness change would not
 * appear until something else redrew. No-op in text mode or single-buffered
 * fallback (there is no separate back buffer to re-scale from). IRQ-safe: it
 * runs to completion before the compositor thread resumes. */
void fb_brightness_step(int dir)
{
	if (!active || single)
		return;
	int nb = brightness + dir;
	if (nb < FB_BRIGHT_MIN) nb = FB_BRIGHT_MIN;
	if (nb > FB_BRIGHT_MAX) nb = FB_BRIGHT_MAX;
	if (nb == brightness)
		return;
	brightness = nb;

	int dim = brightness < FB_BRIGHT_MAX;
	for (uint32_t y = 0; y < fb_h; y++) {
		const uint32_t *s = fb    + y * fb_pitch;
		uint32_t       *d = front + y * fb_pitch;
		for (uint32_t x = 0; x < fb_w; x++)
			d[x] = dim ? dim_px(s[x]) : s[x];
	}
	__asm__ volatile ("sfence" ::: "memory");
	if (present_hook)
		present_hook(0, 0, (int)fb_w, (int)fb_h);
}

int fb_brightness(void) { return brightness; }

int      fb_active(void) { return active; }
uint32_t fb_width(void)  { return fb_w; }
uint32_t fb_height(void) { return fb_h; }
uint32_t *fb_ptr(void)   { return fb; }
uint32_t fb_pitch_pixels(void) { return fb_pitch; }

/* Phase-1 compositor primitives (SYS_FB_*). The back buffer is a contiguous PMM
 * run identity-mapped into the kernel, so its physical base equals fb_ptr(); 0 in
 * text mode or single-buffered fallback (nothing safe to hand a compositor). */
uint32_t fb_backbuffer_phys(void)
{
	return (active && !single) ? (uint32_t)fb : 0;
}

uint32_t fb_backbuffer_bytes(void)
{
	return fb_cap * 4u;
}

void fb_dims(int *w, int *h, int *pitch, int *bpp)
{
	if (w)     *w     = (int)fb_w;
	if (h)     *h     = (int)fb_h;
	if (pitch) *pitch = (int)fb_pitch;    /* pixels per row (stride) */
	if (bpp)   *bpp   = 32;
}

int  fb_target_hz(void) { return target_hz; }
void fb_set_target_hz(int hz)
{
	if (hz < FB_HZ_MIN) hz = FB_HZ_MIN;
	if (hz > FB_HZ_MAX) hz = FB_HZ_MAX;
	target_hz = hz;
}

/* Flush just [x,y,w,h) of the back buffer to VRAM, reusing fb_present's device
 * path (WC drain + SVGA hook). Clipped to the screen; a compositor supplies its
 * own damage rect. */
void fb_present_rect(int x, int y, int w, int h)
{
	if (!active || w <= 0 || h <= 0)
		return;
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w, y1 = y + h;
	if (x1 > (int)fb_w) x1 = (int)fb_w;
	if (y1 > (int)fb_h) y1 = (int)fb_h;
	if (x1 <= x0 || y1 <= y0)
		return;
	dirty_x0 = x0; dirty_y0 = y0; dirty_x1 = x1; dirty_y1 = y1;
	fb_present();
}

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
		fill32(p, color, cw);
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
		memcpy(d, s, (uint32_t)cw * 4);
	}
	mark(cx, cy, cw, ch);
}

/* Blend src (0..255 per channel) over dst by coverage a (0..255). */
static inline uint32_t blend(uint32_t fg, uint32_t bg, unsigned a)
{
	if (a == 0)   return bg;
	if (a == 255) return fg;
	unsigned ia = 255 - a;
	unsigned r = (((fg >> 16) & 0xff) * a + ((bg >> 16) & 0xff) * ia) / 255;
	unsigned g = (((fg >>  8) & 0xff) * a + ((bg >>  8) & 0xff) * ia) / 255;
	unsigned b = ((fg & 0xff) * a + (bg & 0xff) * ia) / 255;
	return (r << 16) | (g << 8) | b;
}

void fb_glyph(int x, int y, unsigned char ch, uint32_t fg, uint32_t bg)
{
	int cx, cy, cw, chh;
	if (!clip(x, y, FONT_W, FONT_H, &cx, &cy, &cw, &chh))
		return;
	int transparent = (bg & FB_TRANSPARENT) != 0;
	for (int row = 0; row < chh; row++) {
		int sy = cy + row;
		const unsigned char *cov = font_aa[ch][sy - y];
		uint32_t *d = fb + sy * (int)fb_pitch;
		for (int col = 0; col < cw; col++) {
			int sx = cx + col;
			unsigned a = cov[sx - x];
			/* Transparent bg: blend the coverage straight over the pixel
			 * already in the (cached) back buffer; opaque bg: fg over bg. */
			d[sx] = transparent ? blend(fg, d[sx], a) : blend(fg, bg, a);
		}
	}
	mark(cx, cy, cw, chh);
}

void fb_text(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
	for (; *s; s++, x += FONT_W)
		fb_glyph(x, y, (unsigned char)*s, fg, bg);
}

/* --- framebuffer boot console -------------------------------------------------
 * A scrolling text console drawn straight to the framebuffer, used during boot
 * (in graphics mode, before the window manager exists) so the verbose bring-up
 * log is visible on screen. In graphics mode the 0xB8000 text the VGA driver
 * writes isn't shown and the compositor isn't up yet, so without this only code
 * that drew the framebuffer directly (the old e1000 fb_line) appeared. vga.c
 * feeds this one char at a time when verbose logging is on; it goes quiet once
 * the WM takes the framebuffer (vga.c stops calling it). */

#define CON_MARGIN 4
static int con_x = CON_MARGIN, con_y = CON_MARGIN;

static void fb_con_newline(void)
{
	con_x = CON_MARGIN;
	con_y += FONT_H;
	if (con_y + FONT_H <= (int)fb_h)
		return;
	/* Scroll the back buffer up one text row and clear the new bottom row. */
	uint32_t move = fb_h - FONT_H;
	for (uint32_t y = 0; y < move; y++)
		for (uint32_t x = 0; x < fb_w; x++)
			fb[y * fb_pitch + x] = fb[(y + FONT_H) * fb_pitch + x];
	for (uint32_t y = move; y < fb_h; y++)
		for (uint32_t x = 0; x < fb_w; x++)
			fb[y * fb_pitch + x] = 0x000000;
	con_y -= FONT_H;
	/* Push the whole back buffer to the visible framebuffer (the scroll touched
	 * every row, so a damage-rect present won't do). single mode draws straight
	 * to `front`, so the back-buffer scroll above is already on screen. */
	if (!single) {
		int dim = brightness < FB_BRIGHT_MAX;
		for (uint32_t y = 0; y < fb_h; y++) {
			const uint32_t *s = fb + y * fb_pitch;
			uint32_t *d = front + y * fb_pitch;
			for (uint32_t x = 0; x < fb_w; x++)
				d[x] = dim ? dim_px(s[x]) : s[x];
		}
		__asm__ volatile ("sfence" ::: "memory");
	}
	if (present_hook)              /* SVGA: the scroll touched the whole screen */
		present_hook(0, 0, (int)fb_w, (int)fb_h);
}

void fb_con_putc(char c)
{
	if (!active)
		return;
	if (c == '\r') return;
	if (c == '\n') { fb_con_newline(); return; }
	if (c == '\t') { do { fb_con_putc(' '); } while (((con_x - CON_MARGIN) / FONT_W) % 8); return; }
	if (c == '\b') { if (con_x > CON_MARGIN) con_x -= FONT_W; return; }
	if (con_x + FONT_W > (int)fb_w)
		fb_con_newline();
	fb_clip_full();
	fb_glyph(con_x, con_y, (unsigned char)c, 0x33ff66, 0x000000);  /* green on black */
	fb_present();                  /* copies just the glyph's damage rect          */
	con_x += FONT_W;
}

/* --- Proportional UI font (sans-serif) for window chrome. Always blends fg
 * over whatever is already in the back buffer (callers fill the bar first), so
 * it has no bg parameter and overlapping advances compose cleanly. --- */

static void fb_ui_glyph(int x, int y, unsigned char ch, uint32_t fg)
{
	int cx, cy, cw, chh;
	if (!clip(x, y, UIFONT_W, UIFONT_H, &cx, &cy, &cw, &chh))
		return;
	for (int row = 0; row < chh; row++) {
		int sy = cy + row;
		const unsigned char *cov = font_ui[ch][sy - y];
		uint32_t *d = fb + sy * (int)fb_pitch;
		for (int col = 0; col < cw; col++) {
			int sx = cx + col;
			unsigned a = cov[sx - x];
			if (a)
				d[sx] = blend(fg, d[sx], a);
		}
	}
	mark(cx, cy, cw, chh);
}

int fb_ui_width(const char *s)
{
	int w = 0;
	for (; *s; s++)
		w += font_ui_adv[(unsigned char)*s];
	return w;
}

void fb_ui_text(int x, int y, const char *s, uint32_t fg)
{
	for (; *s; s++) {
		fb_ui_glyph(x, y, (unsigned char)*s, fg);
		x += font_ui_adv[(unsigned char)*s];
	}
}
