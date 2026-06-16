/* services.c — the service manager.
 *
 * Owns ONLY the registry, the supervisor thread, the lifecycle and the
 * `services` shell command. Every actual daemon lives in its own file under
 * kernel/services/ and registers itself via service_register() from a
 * *_register() function listed in builtins.h. The manager has no per-service
 * knowledge.
 */

#include "services.h"
#include "builtins.h"
#include "sched.h"
#include "timer.h"
#include "console.h"
#include "kstring.h"
#include <stdint.h>

#define MAX_SERVICES 32

static service_t registry[MAX_SERVICES];
static int       n_services;

service_t *service_register(const service_t *tmpl)
{
	if (n_services >= MAX_SERVICES)
		return 0;
	service_t *s = &registry[n_services++];
	*s = *tmpl;
	s->state = SVC_STOPPED;
	s->since = s->work = s->aux = 0;
	return s;
}

/* The built-in services, in registration order. Adding a service is one line
 * here plus its own file — the manager needs no other change. */
static void (*const REGISTRARS[])(void) = {
	smbd_register,
	sshd_register,
	dhcp_register,
	ntp_register,
};

/* --------------------------------------------------------------- lifecycle */

static int svc_start(service_t *s)
{
	if (s->state == SVC_RUNNING)
		return 1;
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
	for (int i = 0; i < n_services; i++)
		if (streq(registry[i].name, name))
			return &registry[i];
	return 0;
}

/* The single supervisor: tick every RUNNING service that wants ticking. */
static void services_supervisor(void)
{
	for (;;) {
		for (int i = 0; i < n_services; i++)
			if (registry[i].state == SVC_RUNNING && registry[i].tick)
				registry[i].tick(&registry[i]);
		sched_yield();
	}
}

void services_init(void)
{
	for (unsigned i = 0; i < sizeof REGISTRARS / sizeof REGISTRARS[0]; i++)
		REGISTRARS[i]();

	sched_create_thread("servicesd", services_supervisor);
	for (int i = 0; i < n_services; i++)
		if (registry[i].thread)
			sched_create_thread(registry[i].name, registry[i].thread);

	int started = 0;
	for (int i = 0; i < n_services; i++)
		if (registry[i].autostart && svc_start(&registry[i]) == 0)
			started++;

	con_write("services: supervisor up, ");
	con_write_dec((uint32_t)started);
	con_write(" auto-started.\n");
}

/* ------------------------------------------------------------------- command */

static void write_field(const char *s, int width)
{
	con_write(s);
	for (int i = (int)strlen(s); i < width; i++)
		con_putc(' ');
}

static void cmd_list(void)
{
	con_write("NAME       PORT   STATE    DESCRIPTION\n");
	for (int i = 0; i < n_services; i++) {
		service_t *s = &registry[i];
		write_field(s->name, 11);
		if (s->port) {
			char p[8]; int n = 0; uint16_t v = s->port;
			char tmp[8]; int t = 0;
			do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
			while (t > 0) p[n++] = tmp[--t];
			p[n] = '\0';
			write_field(p, 7);
		} else {
			write_field("-", 7);
		}
		write_field(s->state == SVC_RUNNING ? "running" : "stopped", 9);
		con_write(s->desc);
		con_putc('\n');
	}
}

static void cmd_status(service_t *s)
{
	con_write(s->name);
	con_write(": ");
	con_write(s->state == SVC_RUNNING ? "running" : "stopped");
	if (s->state == SVC_RUNNING) {
		con_write(", up ");
		con_write_dec(timer_seconds() - s->since);
		con_write("s");
		if (s->port) { con_write(", port "); con_write_dec(s->port); }
		con_write(", activity ");
		con_write_dec(s->work);
	}
	con_putc('\n');
}

void services_cmd(const char *args)
{
	while (*args == ' ')
		args++;

	char verb[16];
	int n = 0;
	while (args[n] && args[n] != ' ' && n < (int)sizeof verb - 1) { verb[n] = args[n]; n++; }
	verb[n] = '\0';
	const char *rest = args + n;
	while (*rest == ' ')
		rest++;

	if (verb[0] == '\0' || streq(verb, "list")) { cmd_list(); return; }

	if (streq(verb, "status") && *rest == '\0') {
		for (int i = 0; i < n_services; i++)
			cmd_status(&registry[i]);
		return;
	}

	if (*rest == '\0') {
		con_write("usage: services [list | start <name> | stop <name> | "
		          "restart <name> | status [name]]\n");
		return;
	}

	service_t *s = svc_find(rest);
	if (!s) {
		con_write("services: no such service: ");
		con_write(rest);
		con_putc('\n');
		return;
	}

	if (streq(verb, "start")) {
		int r = svc_start(s);
		if (r == 0)      con_write("started "), con_write(s->name), con_putc('\n');
		else if (r == 1) con_write(s->name), con_write(" already running\n");
		else             con_write("failed to start "), con_write(s->name), con_putc('\n');
	} else if (streq(verb, "stop")) {
		svc_stop(s);
		con_write("stopped "), con_write(s->name), con_putc('\n');
	} else if (streq(verb, "restart")) {
		svc_stop(s);
		if (svc_start(s) == 0) con_write("restarted "), con_write(s->name), con_putc('\n');
		else                   con_write("failed to restart "), con_write(s->name), con_putc('\n');
	} else if (streq(verb, "status")) {
		cmd_status(s);
	} else {
		con_write("services: unknown subcommand: ");
		con_write(verb);
		con_putc('\n');
	}
}
