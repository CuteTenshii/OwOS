/* es1371.h: Ensoniq AudioPCI ES1371/ES1373 (and CT5880) PCM playback through its AC'97 codec.
 *
 * This is the card VMware emulates ("Sound Card" = es1371) and a common period PCI sound card.
 * es1371_init() is a silent no-op if no card is present. Bring-up busy-waits, so it runs before
 * interrupts are enabled; playback is polled (no IRQ line is claimed), like hda.c.
 */
#ifndef OWOS_ES1371_H
#define OWOS_ES1371_H

#include <stdbool.h>
#include <stdint.h>

void es1371_init(void);
bool es1371_present(void);

/* Master volume on the AC'97 codec, same 0..ES_VOL_MAX scale as sb16_volume_*. */
#define ES_VOL_MAX 8
void es1371_volume_step(int dir);
void es1371_volume_mute(void);
int es1371_volume(void);       /* 0 while muted */
int es1371_volume_level(void); /* ignoring mute */
int es1371_is_muted(void);

/* Streaming PCM sink fed from /bin/play: 48 kHz, S16, stereo, which the DMA buffer carries as is.
 * write() returns bytes consumed, -1 if no stream is open, or -2 on Ctrl+C; close() drains. One stream at a time. */
int es1371_stream_open(void);
int es1371_stream_write(const void *pcm, uint32_t bytes);
void es1371_stream_close(void);

#endif /* OWOS_ES1371_H */
