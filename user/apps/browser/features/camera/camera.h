#ifndef BROWSER_CAMERA_H
#define BROWSER_CAMERA_H

#include "jsint.h"

/* Define navigator.mediaDevices, MediaStream and MediaStreamTrack (secure contexts only). */
void camera_install(js_ctx *ctx, js_object *navigator, const char *page_url);
/* Worker loop: fetch the newest camera frame, notice a camera that went away; returns nonzero if page script ran. */
int camera_tick(void);
/* Page unload: stop the camera, end every track and free the frame. */
void camera_teardown(void);
/* True if `v` is a MediaStream (what HTMLMediaElement.srcObject accepts). */
bool camera_is_stream(js_value v);

#endif
