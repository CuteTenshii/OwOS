/* sshd.h — the SSH-2 server (the `sshd` service). Mirrors smb.h: the daemon
 * thread in services.c accepts a TCP connection on port 22 and hands it here. */
#ifndef OWOS_SSHD_H
#define OWOS_SSHD_H

#include "tcp.h"

/* Serve one SSH connection to completion (key exchange, password auth against
 * /etc/passwd, then a session channel running the user's shell). Synchronous,
 * one client at a time. */
void ssh_serve(tcp_conn_t *c);

/* Register the sshd service with the services manager (called from
 * services_init via builtins.h). sshd owns its own accept loop + lifecycle. */
void sshd_register(void);

#endif /* OWOS_SSHD_H */
