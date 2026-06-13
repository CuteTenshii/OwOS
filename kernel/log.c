/* log.c — the kernel journal (see log.h).
 *
 * A ring buffer plus two policy flags. It deliberately knows nothing about how
 * lines are drawn: vga.c feeds it during boot and consults log_verbose() to
 * decide whether to also paint the line. Keeping the journal here, separate
 * from the console, is what lets the terminal be its own surface while the full
 * boot log stays one command (`journal`) away. */

#include <stddef.h>
#include <stdint.h>

#include "log.h"
#include "console.h"
#include "fb.h"

/* Boot output is a few KiB; 16 KiB leaves headroom. It is a ring: if logging
 * ever outruns the buffer the oldest bytes roll off rather than overflow. */
static char     buf[16 * 1024];
static uint32_t head;            /* next write index                           */
static int      wrapped;         /* set once head has lapped the buffer        */
static int      capturing = 1;   /* recording (on through boot)                */
static int      verbose;         /* also draw the boot log on screen (boot arg) */

void journal_record(char c)
{
	buf[head++] = c;
	if (head == sizeof buf) {
		head = 0;
		wrapped = 1;
	}
}

void journal_capture(int on) { capturing = on ? 1 : 0; }
int  journal_capturing(void) { return capturing; }

void klog(const char *s)
{
	/* Record straight into the ring, then -- only if verbose -- echo to the
	 * console. The echo goes through con_write with capture suspended so the
	 * console funnel doesn't record it a second time (it would, if klog is
	 * called while boot capture is still on). */
	int cap = capturing;
	for (const char *p = s; *p; p++)
		journal_record(*p);
	if (verbose) {
		capturing = 0;
		con_write(s);
		capturing = cap;
	}
}

void klog_dec(unsigned int value)
{
	char tmp[11];
	int n = 0;
	if (value == 0) {
		klog("0");
		return;
	}
	char rev[10];
	int i = 0;
	while (value) {                 /* digits come out least-significant first  */
		rev[i++] = (char)('0' + value % 10);
		value /= 10;
	}
	while (i--)
		tmp[n++] = rev[i];
	tmp[n] = '\0';
	klog(tmp);
}

void log_set_verbose(int on) { verbose = on ? 1 : 0; }
int  log_verbose(void)       { return verbose; }

void journal_to_fb(void)
{
	/* Replay everything recorded so far onto the framebuffer boot console, so the
	 * lines logged before graphics mode came up (which only reached 0xB8000 text)
	 * also appear on screen. Called once, right after the framebuffer is up. */
	if (wrapped)
		for (uint32_t i = head; i < sizeof buf; i++)
			fb_con_putc(buf[i]);
	for (uint32_t i = 0; i < head; i++)
		fb_con_putc(buf[i]);
}

void journal_dump(void)
{
	/* The shell switches capture off before it can run this, so the replay
	 * isn't recorded back into the ring. Print oldest-first: the tail past
	 * `head` (present only after a wrap), then everything up to `head`. */
	if (wrapped)
		for (uint32_t i = head; i < sizeof buf; i++)
			con_putc(buf[i]);
	for (uint32_t i = 0; i < head; i++)
		con_putc(buf[i]);
}
