/* screenshot.c — capture the framebuffer to /home/root/screenshotN.png.
 *
 * Print Screen fires from the keyboard IRQ, which only sets `pending`. A
 * dedicated "screenshot" kernel thread (not the WM thread) does the capture,
 * PNG-encode (png.c, stored zlib blocks — fast, no compression stall) and the
 * multi-megabyte disk write. Keeping it off the compositor thread is what stops
 * Print Screen from freezing the desktop: the timer preempts the worker so the
 * WM keeps painting while the shot is encoded and written.
 *
 * The encode reads the live back buffer in a single pass (that pass *is* the
 * capture); after it, no more framebuffer reads happen, so the only window for
 * tearing is those few milliseconds.
 */
#include "screenshot.h"
#include "fb.h"
#include "png.h"
#include "fs.h"
#include "serial.h"
#include "sched.h"
#include "timer.h"
#include <stdint.h>

static volatile int pending;

void screenshot_request(void) { pending = 1; }

/* Append the decimal form of v to buf at *pos. */
static void put_uint(char *buf, int *pos, uint32_t v)
{
	char tmp[10];
	int n = 0;
	if (v == 0)
		tmp[n++] = '0';
	while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
	while (n)
		buf[(*pos)++] = tmp[--n];
}

/* Build "/home/root/screenshot<idx>.png" into path. */
static void make_path(char *path, uint32_t idx)
{
	const char *pre = "/home/root/screenshot";
	const char *ext = ".png";
	int p = 0;
	for (const char *s = pre; *s; s++) path[p++] = *s;
	put_uint(path, &p, idx);
	for (const char *s = ext; *s; s++) path[p++] = *s;
	path[p] = '\0';
}

/* Do the actual capture + encode + save. Runs on the worker thread. */
static void capture(void)
{
	if (!fb_active())
		return;

	uint32_t w = fb_width(), h = fb_height(), pitch = fb_pitch_pixels();
	const uint32_t *px = fb_ptr();
	if (!w || !h || !px)
		return;

	/* Pick the first free /home/root/screenshotN.png. */
	char path[48];
	uint32_t idx = 0;
	for (;;) {
		make_path(path, idx);
		if (fs_stat(path, 0, 0, 0) != 0)        /* path is free                */
			break;
		if (++idx > 9999)                       /* give up rather than spin     */
			return;
	}

	/* Stream the PNG straight to disk — no multi-megabyte buffer in RAM. */
	int r = png_write_stream(path, px, w, h, pitch);
	if (r != PNG_OK) {
		serial_write("screenshot: ");
		serial_write(png_strerror(r));
		serial_write("\r\n");
		return;
	}
	serial_write("screenshot saved: ");
	serial_write(path);
	serial_write("\r\n");
}

/* The worker: park until a shot is requested, then take it. timer_sleep yields
 * the CPU while idle (no busy-spin) and keeps interrupts enabled. */
static void screenshot_thread(void)
{
	for (;;) {
		if (!pending) {
			timer_sleep(100);
			continue;
		}
		pending = 0;
		capture();
	}
}

void screenshot_init(void)
{
	sched_create_thread("screenshot", screenshot_thread);
}
