#ifndef BROWSER_GAMEPAD_H
#define BROWSER_GAMEPAD_H

#include "jsint.h"

/* Define navigator.getGamepads and the gamepadconnected/gamepaddisconnected events. */
void gamepad_install(js_ctx *ctx, js_object *navigator);
/* Worker loop: poll the pad and fire its events. */
void gamepad_tick(void);
/* Page unload: unroot the page's objects and stop any rumble it started. */
void gamepad_teardown(void);

#endif
