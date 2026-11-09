/* OS declarations the freestanding browser sources use that tests/owos.h lacks. */
void msleep(unsigned ms);
void rng_bytes(void *buf, unsigned long n);
unsigned long rtc_epoch(void);

/* bindings.c builds with realloc -> poison_realloc so reads of uninitialized growth fail */
void *poison_realloc(void *ptr, unsigned long size);

/* PCM sink */
int audio_open(void);
int audio_write(const void *pcm, int len);
int audio_close(void);
