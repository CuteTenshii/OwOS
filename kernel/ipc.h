/* ipc.h — named local ports: a connection-oriented IPC primitive, the
 * unix-socket analog OwOS otherwise lacks (pipes are parent-child only).
 *
 * A server registers a port by name and accept()s connections; a client opens
 * the same name to connect. Each connection is a bidirectional byte channel
 * (two pipe-pool rings) the kernel exposes to both ends as one FD_CHANNEL fd
 * that ordinary read()/write() drive. General by design — any daemon can use
 * it; its first user is /bin/clipd (the clipboard server).
 *
 * The fd wiring (installing FD_PORT / FD_CHANNEL into a task's table) lives in
 * syscall.c; this module owns the port registry and the connection handshake
 * and only deals in pipe-pool ids.
 */
#ifndef OWOS_IPC_H
#define OWOS_IPC_H

#include <stdbool.h>

/* Server side: register a port by name -> port id >=0, or -1 (name taken /
 * full). ipc_port_destroy releases the port and any not-yet-accepted
 * connection. */
int ipc_port_create(const char *name);
void ipc_port_destroy(int port_id);

/* Client connect to a named server. On success returns 0 and fills rx/tx with
 * the client's channel pipe ends (read rx, write tx); -1 if no such server or
 * the pipe pool is exhausted. Blocks while a prior connection awaits accept. */
int ipc_connect(const char *name, int *rx, int *tx);

/* Server accept: wait (unless !block) for a client on `port_id`, then return 0 and fill rx/tx with the server's
 * channel pipe ends; 1 when !block and nobody is waiting; -1 if the port went away. Each side's rx pipe carries the
 * other side's pid/uid/gid (pipe_peer), recorded at connect. */
int ipc_accept(int port_id, int *rx, int *tx, bool block);

#endif /* OWOS_IPC_H */
