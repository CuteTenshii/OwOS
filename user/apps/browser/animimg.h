/* animimg.h - frame playback for animated inline images (GIF, animated WebP). */
#ifndef OWOS_BROWSER_ANIMIMG_H
#define OWOS_BROWSER_ANIMIMG_H

#include "imganim.h"
#include <stdbool.h>

/* Register a player whose first frame is already in the pool under `uid`. Takes ownership of `a` and `data` (the
 * bytes `a` decodes from) on success; returns false, leaving both with the caller, if the table is full. Any thread. */
bool animimg_add(int uid, struct imganim *a, unsigned char *data, unsigned delay_ms);

/* The image with `uid` was just drawn: keeps it playing. UI thread. */
void animimg_seen(int uid);

/* Advance every due image that is on screen, writing its new frame into the pool and damaging its rows. UI thread. */
void animimg_tick(void);

#endif /* OWOS_BROWSER_ANIMIMG_H */
