#ifndef BROWSER_MICROPHONE_H
#define BROWSER_MICROPHONE_H

#include "features/mediastream/mediastream.h"

/* The sound card's capture side as a MediaStream device: signed 16-bit little-endian mono at 48000 Hz. */
const struct ms_device *microphone_device(void);

/* Receives every chunk captured for a track (zeros while the track is disabled), on the worker thread. */
typedef void (*mic_sink)(js_ctx *ctx, int handle, const short *pcm, int n);
/* Subscribe to a live audio track; returns a handle, or 0 if `track` is not live. */
int mic_attach(js_value track, mic_sink fn);
void mic_detach(int handle);

#endif
