/* svc_ntp.c — the NTP clock-sync service. On demand (`services start ntp`): one
 * synchronous SNTP round-trip to set the clock, run from the command/servicesd
 * context with interrupts on. Not a daemon — start triggers the sync. */

#include "services.h"
#include "builtins.h"
#include "net.h"

static int ntp_start(service_t *s) { (void)s; return net_ntp(""); }

void ntp_register(void)
{
	service_register(&(service_t){
		.name = "ntp", .desc = "SNTP clock sync (start to sync)",
		.on_start = ntp_start,
	});
}
