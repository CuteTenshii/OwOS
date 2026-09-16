/* heap.h — a simple kernel heap (kmalloc/kfree). */
#ifndef OWOS_HEAP_H
#define OWOS_HEAP_H

#include <stddef.h>

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

size_t heap_used_bytes(void);
size_t heap_free_bytes(void);

#endif /* OWOS_HEAP_H */
