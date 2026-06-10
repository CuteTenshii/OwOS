/* services.c — supervisor + registry for headless background daemons.
 *
 * Each entry in the table is a service: a name, a one-line description, an
 * optional bound UDP port, and up to three callbacks (on_start / on_stop /
 * tick). Starting a service flips it RUNNING and calls on_start (e.g. to bind a
 * UDP port); stopping it calls on_stop and flips it STOPPED. A single
 * "servicesd" kernel thread ticks every RUNNING service round-robin — that
 * drives time-based daemons (the heartbeat); purely reactive network daemons
 * (echo, daytime) leave tick NULL and do their work from the NIC's RX
 * interrupt, replying with net_udp_send.
 *
 * The model is deliberately the same shape a real SMB/SSH server would use:
 * bind a port in on_start, serve from the receive path, release it in on_stop.
 */

#include "services.h"
#include "sched.h"
#include "timer.h"
#include "net.h"
#include "tcp.h"
#include "smb.h"
#include "rtc.h"
#include "vga.h"
#include <stdint.h>

enum svc_state { SVC_STOPPED = 0, SVC_RUNNING };

typedef struct service {
	const char *name;
	const char *desc;
	uint16_t    port;                 /* bound UDP port (0 = not a net service) */
	uint8_t     autostart;            /* brought up at boot                     */
	uint8_t     state;
	uint32_t    since;                /* timer_seconds() when last started      */
	uint32_t    work;                 /* activity counter (beats / datagrams)   */
	uint32_t    aux;                  /* per-service scratch (heartbeat second) */
	int       (*on_start)(struct service *);   /* 0 = ok, <0 = failed           */
	void      (*on_stop)(struct service *);
	void      (*tick)(struct service *);
} service_t;

/* Fixed registry; handlers reach their own entry through these indices. */
enum { SVC_BEAT, SVC_ECHOD, SVC_DAYTIMED, SVC_ECHOTCP, SVC_SMBD, SVC_COUNT };
static service_t svc[SVC_COUNT];

/* ------------------------------------------------------------ tiny utilities */

static int svc_streq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b)
			return 0;
		a++; b++;
	}
	return *a == *b;
}

static int svc_strlen(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

/* Write `s` left-justified in a field `width` columns wide. */
static void write_field(const char *s, int width)
{
	vga_write(s);
	for (int i = svc_strlen(s); i < width; i++)
		vga_putc(' ');
}

/* Append `v` (zero-padded to `width`, 0 = no padding) as decimal to out[*pos]. */
static void put_u(char *out, int *pos, uint32_t v, int width)
{
	char tmp[10];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n < width)
		tmp[n++] = '0';
	while (n > 0)
		out[(*pos)++] = tmp[--n];
}

/* --------------------------------------------------------------- heartbeat */
/* An always-on daemon with no network presence: once per second it bumps its
 * beat counter, giving the supervisor a visible, testable sign of life. */

static void beat_tick(service_t *s)
{
	uint32_t now = timer_seconds();
	if (now != s->aux) {
		s->aux = now;
		s->work++;
	}
}

/* ------------------------------------------------------------- echo (RFC 862) */
/* Reflects every UDP datagram on port 7 straight back to its sender. */

static void echod_handler(const uint8_t src[4], uint16_t sport,
                          const uint8_t *data, uint32_t len)
{
	if (svc[SVC_ECHOD].state != SVC_RUNNING)
		return;
	net_udp_send(src, sport, 7, data, len);
	svc[SVC_ECHOD].work++;
}

static int echod_start(service_t *s) { return net_udp_listen(s->port, echod_handler); }
static void echod_stop(service_t *s) { net_udp_unlisten(s->port); }

/* ----------------------------------------------------------- daytime (RFC 867) */
/* Answers any UDP datagram on port 13 with the current wall-clock time. */

static int daytime_str(char *out)
{
	struct rtc_time t;
	rtc_now(&t);
	int p = 0;
	put_u(out, &p, t.year, 4);  out[p++] = '-';
	put_u(out, &p, t.month, 2); out[p++] = '-';
	put_u(out, &p, t.day, 2);   out[p++] = ' ';
	put_u(out, &p, t.hour, 2);  out[p++] = ':';
	put_u(out, &p, t.min, 2);   out[p++] = ':';
	put_u(out, &p, t.sec, 2);   out[p++] = '\n';
	return p;
}

static void daytimed_handler(const uint8_t src[4], uint16_t sport,
                             const uint8_t *data, uint32_t len)
{
	(void)data; (void)len;
	if (svc[SVC_DAYTIMED].state != SVC_RUNNING)
		return;
	char buf[24];
	int n = daytime_str(buf);
	net_udp_send(src, sport, 13, (const uint8_t *)buf, (uint32_t)n);
	svc[SVC_DAYTIMED].work++;
}

static int daytimed_start(service_t *s) { return net_udp_listen(s->port, daytimed_handler); }
static void daytimed_stop(service_t *s) { net_udp_unlisten(s->port); }

/* ---------------------------------------------------- echo over TCP (RFC 862) */
/* The TCP counterpart of echod, and the proof that the listen/accept path works
 * end to end: a dedicated thread blocks in tcp_accept(7), echoes the stream
 * back until the peer closes, then loops for the next client. Unlike the UDP
 * daemons it can't serve from interrupt context — tcp_read/tcp_write block — so
 * it owns a thread. The thread is spawned once and parks (yields) whenever the
 * service is stopped; on_stop also trips accept_stop so a blocked accept wakes. */

static volatile int echotcp_stop;

static void echotcp_thread(void)
{
	for (;;) {
		if (svc[SVC_ECHOTCP].state != SVC_RUNNING) {
			sched_yield();
			continue;
		}
		tcp_conn_t *c = tcp_accept(7, &echotcp_stop);
		if (!c) {
			sched_yield();
			continue;
		}
		uint8_t buf[512];
		int closed = 0;
		for (;;) {
			int n = tcp_read(c, buf, sizeof buf, &closed);
			if (n > 0) {
				tcp_write(c, buf, (uint32_t)n);
				svc[SVC_ECHOTCP].work++;
			}
			if (closed || n < 0)
				break;
		}
		tcp_close(c);
	}
}

static int echotcp_start(service_t *s)
{
	echotcp_stop = 0;
	return tcp_listen(s->port);
}

static void echotcp_stop_fn(service_t *s)
{
	tcp_unlisten(s->port);
	echotcp_stop = 1;                       /* unblock a parked tcp_accept */
}

/* ------------------------------------------------------------- SMB (port 445) */
/* The real payload: a read-only SMB2 file server. Same daemon-thread shape as
 * echotcp — accept a TCP connection, hand it to smb_serve() which runs the SMB2
 * conversation to completion, then loop. One client at a time. */

static volatile int smbd_stop;

static void smbd_thread(void)
{
	for (;;) {
		if (svc[SVC_SMBD].state != SVC_RUNNING) {
			sched_yield();
			continue;
		}
		tcp_conn_t *c = tcp_accept(445, &smbd_stop);
		if (!c) {
			sched_yield();
			continue;
		}
		smb_serve(c);
		tcp_close(c);
		svc[SVC_SMBD].work++;
	}
}

static int smbd_start(service_t *s)
{
	smbd_stop = 0;
	return tcp_listen(s->port);
}

static void smbd_stop_fn(service_t *s)
{
	tcp_unlisten(s->port);
	smbd_stop = 1;
}

/* ---------------------------------------------------------------- supervisor */

static void services_supervisor(void)
{
	for (;;) {
		for (int i = 0; i < SVC_COUNT; i++)
			if (svc[i].state == SVC_RUNNING && svc[i].tick)
				svc[i].tick(&svc[i]);
		sched_yield();                 /* cooperatively share the CPU           */
	}
}

/* --------------------------------------------------------------- lifecycle */

static int svc_start(service_t *s)
{
	if (s->state == SVC_RUNNING)
		return 1;                      /* already up                            */
	if (s->on_start && s->on_start(s) < 0)
		return -1;
	s->state = SVC_RUNNING;
	s->since = timer_seconds();
	s->aux = 0;
	return 0;
}

static void svc_stop(service_t *s)
{
	if (s->state != SVC_RUNNING)
		return;
	if (s->on_stop)
		s->on_stop(s);
	s->state = SVC_STOPPED;
}

static service_t *svc_find(const char *name)
{
	for (int i = 0; i < SVC_COUNT; i++)
		if (svc_streq(svc[i].name, name))
			return &svc[i];
	return 0;
}

void services_init(void)
{
	svc[SVC_BEAT] = (service_t){
		.name = "beat", .desc = "heartbeat (1 Hz liveness counter)",
		.port = 0, .autostart = 1, .tick = beat_tick,
	};
	svc[SVC_ECHOD] = (service_t){
		.name = "echod", .desc = "UDP echo server (RFC 862)",
		.port = 7, .on_start = echod_start, .on_stop = echod_stop,
	};
	svc[SVC_DAYTIMED] = (service_t){
		.name = "daytimed", .desc = "UDP daytime server (RFC 867)",
		.port = 13, .on_start = daytimed_start, .on_stop = daytimed_stop,
	};
	svc[SVC_ECHOTCP] = (service_t){
		.name = "echotcp", .desc = "TCP echo server (RFC 862)",
		.port = 7, .on_start = echotcp_start, .on_stop = echotcp_stop_fn,
	};
	svc[SVC_SMBD] = (service_t){
		.name = "smbd", .desc = "SMB2 file server (read-only, guest)",
		.port = 445, .on_start = smbd_start, .on_stop = smbd_stop_fn,
	};

	sched_create_thread("servicesd", services_supervisor);
	sched_create_thread("echotcpd", echotcp_thread);
	sched_create_thread("smbd", smbd_thread);

	int started = 0;
	for (int i = 0; i < SVC_COUNT; i++)
		if (svc[i].autostart && svc_start(&svc[i]) == 0)
			started++;

	vga_write("services: supervisor up, ");
	vga_write_dec((uint32_t)started);
	vga_write(" auto-started.\n");
}

/* ------------------------------------------------------------------- command */

static void print_state(const service_t *s)
{
	if (s->state == SVC_RUNNING)
		vga_write("running");
	else
		vga_write("stopped");
}

static void cmd_list(void)
{
	vga_write("NAME       PORT   STATE    DESCRIPTION\n");
	for (int i = 0; i < SVC_COUNT; i++) {
		service_t *s = &svc[i];
		write_field(s->name, 11);
		if (s->port) {
			char p[8];
			int pp = 0;
			put_u(p, &pp, s->port, 0);
			p[pp] = '\0';
			write_field(p, 7);
		} else {
			write_field("-", 7);
		}
		write_field(s->state == SVC_RUNNING ? "running" : "stopped", 9);
		vga_write(s->desc);
		vga_putc('\n');
	}
}

static void cmd_status(service_t *s)
{
	vga_write(s->name);
	vga_write(": ");
	print_state(s);
	if (s->state == SVC_RUNNING) {
		vga_write(", up ");
		vga_write_dec(timer_seconds() - s->since);
		vga_write("s");
		vga_write(s->port ? ", port " : "");
		if (s->port)
			vga_write_dec(s->port);
		vga_write(", activity ");
		vga_write_dec(s->work);
	}
	vga_putc('\n');
}

void services_cmd(const char *args)
{
	while (*args == ' ')
		args++;

	/* split off the first word (the subcommand) */
	char verb[16];
	int n = 0;
	while (args[n] && args[n] != ' ' && n < (int)sizeof(verb) - 1) {
		verb[n] = args[n];
		n++;
	}
	verb[n] = '\0';
	const char *rest = args + n;
	while (*rest == ' ')
		rest++;

	if (verb[0] == '\0' || svc_streq(verb, "list")) {
		cmd_list();
		return;
	}

	if (svc_streq(verb, "status") && *rest == '\0') {
		for (int i = 0; i < SVC_COUNT; i++)
			cmd_status(&svc[i]);
		return;
	}

	if (*rest == '\0') {
		vga_write("usage: services [list | start <name> | stop <name> | "
		          "restart <name> | status [name]]\n");
		return;
	}

	service_t *s = svc_find(rest);
	if (!s) {
		vga_write("services: no such service: ");
		vga_write(rest);
		vga_putc('\n');
		return;
	}

	if (svc_streq(verb, "start")) {
		int r = svc_start(s);
		if (r == 0)      vga_write("started "), vga_write(s->name), vga_putc('\n');
		else if (r == 1) vga_write(s->name), vga_write(" already running\n");
		else             vga_write("failed to start "), vga_write(s->name), vga_putc('\n');
	} else if (svc_streq(verb, "stop")) {
		svc_stop(s);
		vga_write("stopped "), vga_write(s->name), vga_putc('\n');
	} else if (svc_streq(verb, "restart")) {
		svc_stop(s);
		if (svc_start(s) == 0)
			vga_write("restarted "), vga_write(s->name), vga_putc('\n');
		else
			vga_write("failed to restart "), vga_write(s->name), vga_putc('\n');
	} else if (svc_streq(verb, "status")) {
		cmd_status(s);
	} else {
		vga_write("services: unknown subcommand: ");
		vga_write(verb);
		vga_putc('\n');
	}
}
