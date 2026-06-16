/* svc_smbd.c — the SMB2 file server service. A dedicated thread blocks in
 * tcp_accept(445), hands each connection to smb_serve() (kernel/smb.c) which
 * runs the SMB2 conversation to completion, then loops. One client at a time. */

#include "services.h"
#include "builtins.h"
#include "tcp.h"
#include "smb.h"
#include "sched.h"

static service_t  *self;
static volatile int stop;

static void smbd_thread(void)
{
	for (;;) {
		if (!self || self->state != SVC_RUNNING) { sched_yield(); continue; }
		tcp_conn_t *c = tcp_accept(445, &stop);
		if (!c) { sched_yield(); continue; }
		smb_serve(c);
		tcp_close(c);
		self->work++;
	}
}

static int smbd_start(service_t *s) { stop = 0; return tcp_listen(s->port); }
static void smbd_stop(service_t *s) { tcp_unlisten(s->port); stop = 1; }

void smbd_register(void)
{
	self = service_register(&(service_t){
		.name = "smbd", .desc = "SMB2 file server (read-only, guest)",
		.port = 445, .on_start = smbd_start, .on_stop = smbd_stop, .thread = smbd_thread,
	});
}
