/* vmtools.c — VMware Tools guest agent (backdoor). See vmtools.h.
 *
 * QEMU can't exercise this (its vmport has no message channel), so it carries
 * journal logging for on-VMware debugging, the way battery.c does for laptops.
 * Verify on a real VMware guest:
 *   - clock:     boots with the host's time even with no RTC battery / no NTP
 *   - guestinfo: host `vmrun readVariable <vmx> guestVar owos.version` prints it
 *   - heartbeat: the VM reports VMware Tools as running/alive
 */

#include <stdint.h>
#include "vmtools.h"
#include "vmware.h"
#include "kclip.h"
#include "rtc.h"
#include "console.h"
#include "sched.h"
#include "timer.h"

#define BDOOR_CMD_GETTIME 14       /* host UTC seconds in EAX, microseconds in EBX */

#define HEARTBEAT_MS 30000

#define CLIP_POLL_MS  1000         /* how often to check the host clipboard       */
#define CLIP_MAX      8192         /* clipboard capacity (text only)              */

/* The shared clipboard. clip_len is published last on every update and gates all
 * reads, so a reader (vmtools_clip_get, possibly in the keyboard IRQ) always
 * sees a consistent buffer/length pair without locking — single-writer here is
 * either the poll thread (host->guest) or vmtools_clip_set (guest copy). */
static char         clip_buf[CLIP_MAX];
static volatile int clip_len;

void vmtools_clip_set(const char *s, int len)
{
	if (len < 0)
		len = 0;
	if (len > CLIP_MAX)
		len = CLIP_MAX;
	for (int i = 0; i < len; i++)
		clip_buf[i] = s[i];
	clip_len = len;
	vmware_clip_send(clip_buf, clip_len);   /* push to host; -1/no-op off VMware  */
}

int vmtools_clip_get(char *out, int max)
{
	int n = clip_len;
	if (n > max)
		n = max;
	for (int i = 0; i < n; i++)
		out[i] = clip_buf[i];
	return n;
}

static int clip_same(const char *a, const char *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i] != b[i]) return 0;
	return 1;
}

/* Bridge the VMware host clipboard and clipd (the system clipboard): a host copy
 * becomes the newest clip, and a guest copy is offered to the host. Text only —
 * the host backdoor channel carries text. `last` remembers whatever text we last
 * bridged in either direction, so the two sides don't ping-pong an echo forever.
 * Real-VMware only; vmware_clip_recv/send are no-ops (-1) off VMware, so this
 * idles harmlessly there. */
static void vmclip_thread(void)
{
	static char scratch[CLIP_MAX];
	static char last[CLIP_MAX];
	int last_len = 0;
	for (;;) {
		timer_sleep(CLIP_POLL_MS);

		/* host -> clipd: the host offered a new selection. */
		int n = vmware_clip_recv(scratch, sizeof scratch);
		if (n > 0 && (n != last_len || !clip_same(scratch, last, n))) {
			kclip_set_text(scratch, n);
			for (int i = 0; i < n; i++) last[i] = scratch[i];
			last_len = n;
			continue;                       /* don't echo it straight back to host */
		}

		/* clipd -> host: a guest copy changed the newest text clip. */
		int m = kclip_get_text(scratch, sizeof scratch);
		if (m > 0 && (m != last_len || !clip_same(scratch, last, m))) {
			vmware_clip_send(scratch, m);
			for (int i = 0; i < m; i++) last[i] = scratch[i];
			last_len = m;
		}
	}
}

/* Convert Unix seconds (UTC) to a CMOS clock value (Howard Hinnant's civil-from-
 * days). The host hands us UTC; we store UTC, same as the NTP path's default. */
static void unix_to_rtc(uint32_t t, struct rtc_time *o)
{
	uint32_t rem = t % 86400u;
	uint32_t days = t / 86400u;
	o->hour = (uint8_t)(rem / 3600u);
	rem %= 3600u;
	o->min = (uint8_t)(rem / 60u);
	o->sec = (uint8_t)(rem % 60u);

	int32_t  z   = (int32_t)days + 719468;
	int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
	uint32_t doe = (uint32_t)(z - era * 146097);
	uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint32_t mp  = (5 * doy + 2) / 153;
	uint32_t d   = doy - (153 * mp + 2) / 5 + 1;
	uint32_t m   = mp < 10 ? mp + 3 : mp - 9;
	int32_t  y   = (int32_t)yoe + era * 400 + (m <= 2);

	o->year  = (uint16_t)y;
	o->month = (uint8_t)m;
	o->day   = (uint8_t)d;
}

/* Pull the host wall-clock through the backdoor and set the CMOS clock. */
static int sync_time(void)
{
	struct vmware_regs r = { 0 };
	vmware_send(BDOOR_CMD_GETTIME, &r);

	uint32_t secs = r.eax;
	if (secs == 0 || secs == 0xFFFFFFFFu || secs < 1500000000u)
		return -1;                 /* unsupported / implausible (pre-2017)        */

	struct rtc_time t;
	unix_to_rtc(secs, &t);
	rtc_set(&t);
	return 0;
}

/* Periodic liveness: re-assert guestinfo and the tools version so the host keeps
 * seeing the guest as alive. Only started when the RPC channel actually works. */
static void vmtools_thread(void)
{
	for (;;) {
		timer_sleep(HEARTBEAT_MS);
		vmware_rpc("tools.set.version 0", 0, 0);
		vmware_rpc("info-set guestinfo.owos.alive 1", 0, 0);
	}
}

void vmtools_init(void)
{
	if (!vmware_present())
		return;                    /* not a VMware guest: stay silent             */

	con_write("vmtools: VMware backdoor present.\n");

	if (sync_time() == 0)
		con_write("vmtools: clock synced from host.\n");
	else
		con_write("vmtools: host time-sync unavailable.\n");

	/* Clipboard sync rides the plain backdoor (commands 6-9), not the GuestRPC
	 * message channel, so start it for any VMware guest — even one where the
	 * heartbeat RPC below turns out to be unavailable. */
	sched_create_thread("vmclip", vmclip_thread);

	/* Publish a guest variable and announce the tools version. info-set lands in
	 * guestinfo.* which the host can read back; its success tells us whether the
	 * GuestRPC channel (real VMware) is live or absent (QEMU). */
	int rc = vmware_rpc("info-set guestinfo.owos.version OwOS", 0, 0);
	if (rc < 0) {
		con_write("vmtools: GuestRPC channel unavailable (no heartbeat).\n");
		return;
	}
	con_write("vmtools: guestinfo published; GuestRPC channel OK.\n");

	vmware_rpc("tools.set.version 0", 0, 0);
	sched_create_thread("vmtools", vmtools_thread);
}
