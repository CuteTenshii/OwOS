/* apps.h — desktop app registry.
 *
 * Every desktop app is now a /bin program. apps_start() opens the
 * terminal (/bin/term, a user-space shell) at boot; the rest launch on demand
 * from the taskbar Start menu as detached user processes (the WM reaps them).
 *
 * To add an app: drop its program in /bin and add one row to app_menu[] in apps.c.
 */
#ifndef OWOS_APPS_H
#define OWOS_APPS_H

void apps_start(void);   /* open the boot window(s); called by wm_init()       */

/* Start-menu registry, queried by the window manager's launcher. */
int         apps_menu_count(void);
const char *apps_menu_name(int i);
void        apps_menu_launch(int i);

#endif
