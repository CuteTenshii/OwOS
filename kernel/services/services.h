/* services.h — the service manager: a registry of headless background daemons.
 *
 * A "service" is a long-lived kernel daemon with no controlling shell (network
 * servers and the like). Each service lives in its *own* file under
 * kernel/services/ and registers itself with service_register(); the manager
 * (services.c) owns only the registry, the supervisor thread, the lifecycle
 * (start/stop/autostart) and the `services` shell command. It knows nothing
 * about any individual service.
 *
 * A service supplies callbacks:
 *   on_start  bind a port / kick off an action      (0 ok, <0 fail)
 *   on_stop   release the port / stop the daemon
 *   tick      periodic work, called by the supervisor while RUNNING (optional)
 *   thread    a dedicated kernel thread, spawned once at boot (optional; for
 *             stream servers that block in tcp_accept/tcp_read)
 * Purely reactive UDP daemons need none of tick/thread — they serve from the
 * NIC RX interrupt via the net.c UDP hooks.
 */
#ifndef OWOS_SERVICES_H
#define OWOS_SERVICES_H

#include <stdint.h>

enum svc_state { SVC_STOPPED = 0, SVC_RUNNING };

typedef struct service {
	const char *name;
	const char *desc;
	uint16_t    port;                 /* well-known port (0 = not a net service) */
	uint8_t     autostart;            /* brought up at boot                      */
	uint8_t     state;                /* enum svc_state                          */
	uint32_t    since;                /* timer_seconds() when last started       */
	uint32_t    work;                 /* activity counter (beats / requests)     */
	uint32_t    aux;                  /* per-service scratch                     */
	int       (*on_start)(struct service *);
	void      (*on_stop)(struct service *);
	void      (*tick)(struct service *);
	void      (*thread)(void);        /* dedicated accept/serve loop (optional)  */
} service_t;

/* Register a service from a template. Copies it into the registry and returns
 * the persistent entry (a service stashes this to reach its own state/work).
 * Returns NULL if the registry is full. Call from a *_register() function at
 * services_init time. */
service_t *service_register(const service_t *tmpl);

void services_init(void);            /* register built-ins, spawn threads, autostart */
void services_cmd(const char *args); /* `services [list|start|stop|restart|status]`  */

#endif /* OWOS_SERVICES_H */
