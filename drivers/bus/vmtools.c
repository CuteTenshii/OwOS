/* vmtools.c — VMware Tools guest agent (backdoor). See vmtools.h.
 *
 * QEMU can't exercise this (its vmport has no message channel), so it carries
 * journal logging for on-VMware debugging, the way battery.c does for laptops.
 * Verify on a real VMware guest:
 *   - clock:     boots with the host's time even with no RTC battery / no NTP
 *   - guestinfo: host `vmrun readVariable <vmx> guestVar owos.version` prints
 * it
 *   - heartbeat: the VM reports VMware Tools as running/alive
 */

#include "vmtools.h"
#include "console.h"
#include "fb.h"
#include "rtc.h"
#include "sched.h"
#include "timer.h"
#include "vmware.h"
#include <stdbool.h>
#include <stdint.h>

#define BDOOR_CMD_GETTIME                                                                                              \
  14 /* host UTC seconds in EAX, microseconds in EBX                                                                   \
      */

#define HEARTBEAT_MS 30000
#define TCLO_POLL_MS 100

/* Convert Unix seconds (UTC) to a CMOS clock value (Howard Hinnant's
 * civil-from- days). The host hands us UTC; we store UTC, same as the NTP
 * path's default. */
static void unix_to_rtc(uint32_t t, struct rtc_time *o)
{
  uint32_t rem = t % 86400u;
  uint32_t days = t / 86400u;
  o->hour = (uint8_t)(rem / 3600u);
  rem %= 3600u;
  o->min = (uint8_t)(rem / 60u);
  o->sec = (uint8_t)(rem % 60u);

  int32_t z = (int32_t)days + 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  int32_t y = (int32_t)yoe + era * 400 + (m <= 2);

  o->year = (uint16_t)y;
  o->month = (uint8_t)m;
  o->day = (uint8_t)d;
}

/* Pull the host wall-clock through the backdoor and set the CMOS clock. */
static int sync_time(void)
{
  struct vmware_regs r = {0};
  vmware_send(BDOOR_CMD_GETTIME, &r);

  uint32_t secs = r.eax;
  if (secs == 0 || secs == 0xFFFFFFFFu || secs < 1500000000u)
    return -1; /* unsupported / implausible (pre-2017)        */

  struct rtc_time t;
  unix_to_rtc(secs, &t);
  rtc_set(&t);
  return 0;
}

static bool starts_with(const char *s, const char *prefix)
{
  while (*prefix)
    if (*s++ != *prefix++)
      return false;
  return true;
}

static uint32_t parse_uint(const char **p)
{
  uint32_t v = 0;
  while (**p == ' ')
    (*p)++;
  while (**p >= '0' && **p <= '9')
    v = v * 10 + (uint32_t)(*(*p)++ - '0');
  return v;
}

/* Answer one host request: the display follows the VM window via "Resolution_Set w h". */
static void handle_request(const char *req)
{
  if (starts_with(req, "reset")) {
    vmware_tclo_reply("OK ATTACH");
  } else if (starts_with(req, "ping")) {
    vmware_tclo_reply("OK ");
  } else if (starts_with(req, "Capabilities_Register")) {
    vmware_rpc("tools.capability.resolution_set 1", 0, 0);
    vmware_tclo_reply("OK ");
  } else if (starts_with(req, "Resolution_Set")) {
    const char *p = req + 14;
    uint32_t w = parse_uint(&p), h = parse_uint(&p);
    if (w && h)
      fb_request_size(w, h);
    vmware_tclo_reply("OK ");
  } else {
    vmware_tclo_reply("ERROR Unknown command");
  }
}

/* Poll host requests and keep the guest looking alive: re-assert guestinfo and the tools version every
 * HEARTBEAT_MS. Only started when the RPC channel actually works. */
static void vmtools_thread(void)
{
  char req[128];
  uint32_t since_beat = 0;
  for (;;) {
    timer_sleep(TCLO_POLL_MS);
    if (vmware_tclo_poll(req, sizeof req) > 0)
      handle_request(req);
    since_beat += TCLO_POLL_MS;
    if (since_beat >= HEARTBEAT_MS) {
      since_beat = 0;
      vmware_rpc("tools.set.version 0", 0, 0);
      vmware_rpc("info-set guestinfo.owos.alive 1", 0, 0);
    }
  }
}

void vmtools_init(void)
{
  if (!vmware_present())
    return; /* not a VMware guest: stay silent             */

  con_write("vmtools: VMware backdoor present.\n");

  if (sync_time() == 0)
    con_write("vmtools: clock synced from host.\n");
  else
    con_write("vmtools: host time-sync unavailable.\n");

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
