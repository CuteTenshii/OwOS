/* smb.h — a tiny read-only SMB2 file server (see smb.c).
 *
 * Speaks just enough of SMB2 (dialect 2.0.2) over direct TCP/445 for a modern
 * client — Linux `smbclient`/`mount -t cifs`, Windows Explorer — to negotiate,
 * set up a guest session, connect to a single read-only share backed by owofs,
 * list it, and read files. There are no sockets in this kernel, so the server
 * is driven by a daemon thread that hands each accepted TCP connection to
 * smb_serve(); see kernel/services.c (the `smbd` service) and tcp.c's
 * listen/accept path.
 */
#ifndef OWOS_SMB_H
#define OWOS_SMB_H

#include "tcp.h"

/* Serve one accepted SMB2 connection until the peer closes or errors. Runs in
 * the smbd daemon thread (command context), so it may block in tcp_read. */
void smb_serve(tcp_conn_t *c);

#endif /* OWOS_SMB_H */
