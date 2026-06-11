/* apps.c — desktop app registry (see apps.h).
 *
 * apps_start() opens only the terminal at boot; the Start menu launches the
 * rest on demand. Kernel-side apps create their window via their *_init();
 * ring-3 apps are spawned from /bin as detached user processes — their parent
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
	uint8_t *buf = kmalloc(64 * 1024);
	if (!buf)
		return;
	char path[64];
	int j = 0;
	const char *pfx = "/bin/";
	for (int i = 0; pfx[i]; i++)
		path[j++] = pfx[i];
	for (int i = 0; name[i] && j < (int)sizeof(path) - 1; i++)
		path[j++] = name[i];
	path[j] = '\0';

	int n = fs_read(path, buf, 64 * 1024);
	char *argv[1];
	argv[0] = (char *)name;
	if (n >= 0)
		exec_program(buf, (uint32_t)n, 1, argv);
	kfree(buf);
}

static void launch_editor(void)  { launch_bin("editor"); }
static void launch_demo(void)    { launch_bin("demo"); }
static void launch_files(void)   { launch_bin("files"); }
static void launch_clock(void)   { launch_bin("clock"); }
static void launch_about(void)   { launch_bin("about"); }
static void launch_browser(void) { launch_bin("browser"); }

/* The Start menu. Every entry spawns a ring-3 program from /bin. */
static const struct { const char *name; void (*launch)(void); } app_menu[] = {
	{ "Files",   launch_files     },
	{ "Editor",  launch_editor    },
	{ "Browser", launch_browser   },
	{ "Clock",   launch_clock     },
	{ "About",   launch_about     },
	{ "Demo",    launch_demo      },
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
	app_term_init();   /* only the terminal opens at boot; the rest via Start */
}
