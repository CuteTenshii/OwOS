/* svc_sshd.c — the SSH-2 server service. The protocol lives in kernel/sshd.c
 * (ssh_serve); this is just its service wrapper, mirroring svc_smbd.c: a thread
 * blocks in tcp_accept(22), hands each connection to ssh_serve(), then loops.
 * One client at a time. */

#include "services.h"
#include "builtins.h"
#include "tcp.h"
#include "sshd.h"
#include "sched.h"

static service_t  *self;
static volatile int stop;

static void sshd_thread(void)
{
	for (;;) {
		if (!self || self->state != SVC_RUNNING) { sched_yield(); continue; }
		tcp_conn_t *c = tcp_accept(22, &stop);
		if (!c) { sched_yield(); continue; }
		ssh_serve(c);
		tcp_close(c);
		self->work++;
	}
}

static int sshd_start(service_t *s) { stop = 0; return tcp_listen(s->port); }
static void sshd_stop(service_t *s) { tcp_unlisten(s->port); stop = 1; }

void sshd_register(void)
{
	self = service_register(&(service_t){
		.name = "sshd", .desc = "SSH-2 server (ed25519 host key, password auth)",
		.port = 22, .autostart = 1,
		.on_start = sshd_start, .on_stop = sshd_stop, .thread = sshd_thread,
	});
}
