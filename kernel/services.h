/* services.h — a tiny service manager for headless background daemons.
 *
 * Services are long-lived kernel-side daemons (network servers and the like)
 * that run without a controlling shell. A single supervisor thread ticks every
 * running service; network services additionally field traffic from interrupt
 * context via the UDP server hooks in net.c. Services flagged to auto-start are
 * brought up at boot, so they need no `run`/CLI invocation — the shell's
 * `services` command (start/stop/status/list) only manages them after the fact.
 */
#ifndef OWOS_SERVICES_H
#define OWOS_SERVICES_H

void services_init(void);            /* register built-ins, spawn the supervisor,
                                      * and start the auto-start services       */
void services_cmd(const char *args); /* `services [list|start|stop|status] ...` */

#endif /* OWOS_SERVICES_H */
