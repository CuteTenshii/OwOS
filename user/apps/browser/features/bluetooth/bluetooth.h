#ifndef BROWSER_BLUETOOTH_H
#define BROWSER_BLUETOOTH_H

#include "jsint.h"

/* Define navigator.bluetooth and BluetoothUUID (secure contexts only). */
void bluetooth_install(js_ctx *ctx, js_object *navigator, const char *page_url);
/* Worker loop: scan, connection and GATT progress; returns nonzero if page script ran. */
int bluetooth_tick(void);
/* Page unload: stop a scan, drop the connection and unroot the page's objects. */
void bluetooth_teardown(void);

#endif
