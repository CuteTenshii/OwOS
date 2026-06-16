/* svc_dhcp.c — the DHCP client service. Its start *triggers* a lease rather than
 * running a daemon: net_dhcp_start() spawns the async client thread (net.c), so
 * it never blocks bring-up. Autostarts at boot; `services restart dhcp` re-leases. */

#include "services.h"
#include "builtins.h"
#include "net.h"

static int dhcp_start(service_t *s) { (void)s; net_dhcp_start(); return 0; }

void dhcp_register(void)
{
	service_register(&(service_t){
		.name = "dhcp", .desc = "DHCP client (lease IP/gw/dns)",
		.autostart = 1, .on_start = dhcp_start,
	});
}
