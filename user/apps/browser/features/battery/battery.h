#ifndef BROWSER_BATTERY_H
#define BROWSER_BATTERY_H

#include "jsint.h"

/* Define navigator.getBattery (secure contexts only). */
void battery_install(js_ctx *ctx, js_object *navigator, const char *page_url);
/* Worker loop: poll the battery and fire BatteryManager events. */
void battery_tick(void);
/* Page unload: unroot the page's objects. */
void battery_teardown(void);

#endif
