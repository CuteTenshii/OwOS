/* apps.h — desktop app registry.
 *
 * Only the terminal (which hosts the shell) opens at boot. Everything else is
 * launched on demand from the taskbar Start menu — see the registry below. A
 * kernel-side app's launch is its *_init() (it creates a window); a ring-3 app
 * is spawned from /bin as a detached user process (the WM reaps it).
 *
 * To add an app: write its launcher (kernel/app_*.c init, or a /bin spawn) and
 * add one row to app_menu[] in apps.c.
 */
#ifndef OWOS_APPS_H
#define OWOS_APPS_H

void apps_start(void);   /* open the boot window(s); called by wm_init()       */

/* The only kernel-side app left: the terminal, which hosts the in-kernel shell.
 * Every other app is now a ring-3 /bin program launched from the Start menu. */
void app_term_init(void);

/* Start-menu registry, queried by the window manager's launcher. */
int         apps_menu_count(void);
const char *apps_menu_name(int i);
void        apps_menu_launch(int i);

#endif
