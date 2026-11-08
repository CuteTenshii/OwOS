/* OS declarations the freestanding browser sources use that scripts/owos.h lacks. */
void msleep(unsigned ms);
void rng_bytes(void *buf, unsigned long n);
unsigned long rtc_epoch(void);

/* bindings.c builds with realloc -> poison_realloc so reads of uninitialized growth fail */
void *poison_realloc(void *ptr, unsigned long size);
