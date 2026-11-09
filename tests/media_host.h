/* Force-included when media.c is built on the host: the guest-only syscalls it
 * calls (the mixer path), defined as stubs by media_seek_test.c. */
int audio_open(void);
int audio_write(const void *pcm, int len);
int audio_close(void);
int thread_create(void (*fn)(void), void *stack_top);
