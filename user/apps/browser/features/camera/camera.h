#ifndef BROWSER_CAMERA_H
#define BROWSER_CAMERA_H

#include "features/mediastream/mediastream.h"

/* The USB camera as a MediaStream device; camera_frame (bmedia.h) hands its newest picture to <video> and canvas. */
const struct ms_device *camera_device(void);

#endif
