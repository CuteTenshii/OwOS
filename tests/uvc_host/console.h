/* Host stand-in for kernel/console.h (see uvc_stream_test.c). */
#ifndef UVC_HOST_CONSOLE_H
#define UVC_HOST_CONSOLE_H

#define con_printf(...) ((void)0)
#define con_write(s) ((void)(s))

#endif
