#ifndef BROWSER_JS_NOTIFICATION_H
#define BROWSER_JS_NOTIFICATION_H

#include "jsint.h"

void js_notification_setup(js_ctx *ctx, js_object *win, js_object *proto, js_object *queue);
void js_notification_teardown(void);

#endif
