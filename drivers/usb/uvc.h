/* uvc.h: the camera as seen by sysfs (/sys/class/video/cam0). */
#ifndef OWOS_UVC_H
#define OWOS_UVC_H

#include <stdbool.h>
#include <stdint.h>

bool uvc_present(void);

/* name, id and one "format FOURCC WxH fps..." line per frame size. */
int uvc_info_text(char *buf, int max);
/* "idle" or "streaming FOURCC w h fps". */
int uvc_control_text(char *buf, int max);
/* "start FOURCC w h fps" or "stop"; blocks until the poll thread has applied it. 0 on success. */
int uvc_command(const char *line);

/* Stream of a 24-byte little-endian header {seq, fourcc, w, h, payload_len, 0} and the payload of the newest frame.
 * A read at offset 0 latches that frame; later offsets read the latched one. */
uint32_t uvc_frame_size(void);
int uvc_frame_read(uint32_t off, void *buf, uint32_t len);

#endif /* OWOS_UVC_H */
