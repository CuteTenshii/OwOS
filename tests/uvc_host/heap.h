/* Host stand-in for kernel/heap.h: counted so tests can check for leaks. */
#ifndef UVC_HOST_HEAP_H
#define UVC_HOST_HEAP_H

#include <stddef.h>

void *uvc_host_alloc(size_t n);
void uvc_host_free(void *p);
#define kmalloc uvc_host_alloc
#define kfree uvc_host_free

#endif
