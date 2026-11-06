/* sb16.h — Sound Blaster 16 driver: 8-bit PCM playback via ISA DMA.
 *
 * The card lives at the legacy ISA base 0x220, IRQ 5, 8-bit DMA channel 1
 * (QEMU's defaults for `-device sb16`). sb16_init() probes for it with a DSP
 * reset and is a no-op if absent, so booting without the device is harmless.
 */
#ifndef OWOS_SB16_H
#define OWOS_SB16_H

#include <stdint.h>

void sb16_init(void);   /* probe + claim IRQ 5 (silent if no card)     */
int sb16_present(void); /* 1 if a card was detected                     */

/* Master-volume control, driven by the keyboard's media keys (Fn / volume
 * keys). Levels run 0..SB_VOL_MAX; the SB16 mixer attenuates the analog output
 * so a change takes effect immediately, even mid-playback. Safe to call from an
 * IRQ (they only poke the mixer ports). No-ops if no card is present. */
#define SB_VOL_MAX 8
void sb16_volume_step(int dir); /* +1 = louder, -1 = quieter (clamped)        */
void sb16_volume_mute(void);    /* toggle mute (restores the prior level)      */
int sb16_volume(void);          /* 0..SB_VOL_MAX, or 0 while muted             */
int sb16_volume_level(void);    /* 0..SB_VOL_MAX, ignoring mute                */
int sb16_is_muted(void);        /* 1 while muted                               */

/* Streaming PCM sink, fed from /bin/play over the SYS_AUDIO_* calls
 * when no HD Audio codec is present. The producer pushes the canonical 48 kHz/
 * 16-bit/stereo stream; this back end down-converts each frame to 8-bit
 * unsigned mono for the SB16 DAC and paces refills to the auto-init DMA's
 * half-boundary IRQs. write() returns bytes consumed, -1 if no stream is open,
 * or -2 on Ctrl+C; close() drains and stops. One stream at a time. */
int sb16_stream_open(void);
int sb16_stream_write(const void *pcm, uint32_t bytes);
void sb16_stream_close(void);

#endif /* OWOS_SB16_H */
