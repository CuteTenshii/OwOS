/* apps.c — desktop app registry (see apps.h).
 *
 * apps_start() opens only the terminal at boot; the Start menu launches the
 * rest on demand. Kernel-side apps create their window via their *_init();
 * apps are spawned from /bin as detached user processes — their parent
 * is the WM thread (the launcher's caller), which reaps them.
 */
#include <stdint.h>
#include "apps.h"
#include "loader.h"
#include "fs.h"
#include "heap.h"

/* Spawn /bin/<name> as a detached user process (no wait; the WM reaps it). */
static void launch_bin(const char *name)
{
	char path[64];
	int j = 0;
	const char *pfx = "/bin/";
	for (int i = 0; pfx[i]; i++)
		path[j++] = pfx[i];
	for (int i = 0; name[i] && j < (int)sizeof(path) - 1; i++)
		path[j++] = name[i];
	path[j] = '\0';

	/* Size the buffer to the file -- programs are 120+ KiB now, so a fixed 64 KiB
	 * buffer would truncate the ELF and the load would silently fail. */
	int sz = fs_size(path);
	if (sz < 0)
		return;
	uint8_t *buf = kmalloc((uint32_t)sz);
	if (!buf)
		return;
	int n = fs_read(path, buf, (uint32_t)sz);
	char *argv[1];
	argv[0] = (char *)name;
	if (n >= 0)
		exec_program(buf, (uint32_t)n, 1, argv);
	kfree(buf);
}

static void launch_editor(void)  { launch_bin("editor"); }
static void launch_files(void)   { launch_bin("files"); }
static void launch_clock(void)   { launch_bin("clock"); }
static void launch_about(void)   { launch_bin("about"); }
static void launch_browser(void) { launch_bin("browser"); }
static void launch_term(void)    { launch_bin("term"); }
static void launch_player(void)  { launch_bin("player"); }
static void launch_settings(void){ launch_bin("settings"); }

/* The Start menu. Every entry spawns a program from /bin. */
static const struct { const char *name; void (*launch)(void); } app_menu[] = {
	{ "Terminal", launch_term     },
	{ "Files",   launch_files     },
	{ "Editor",  launch_editor    },
	{ "Browser", launch_browser   },
	{ "Media Player", launch_player },
	{ "Clock",   launch_clock     },
	{ "Settings", launch_settings },
	{ "About",   launch_about     },
};

int apps_menu_count(void)
{
	return (int)(sizeof(app_menu) / sizeof(app_menu[0]));
}

const char *apps_menu_name(int i)
{
	return (i >= 0 && i < apps_menu_count()) ? app_menu[i].name : "";
}

void apps_menu_launch(int i)
{
	if (i >= 0 && i < apps_menu_count())
		app_menu[i].launch();
}

void apps_start(void)
{
	/* The terminal is now the /bin/term (a real user-space shell), opened
	 * at boot like a Start-menu app. The old kernel-side app_term window is no
	 * longer created; the kernel shell (shell.c) still drives the serial console
	 * for `make debug`. */
	launch_bin("term");
}
