/* sb16.h — Sound Blaster 16 driver: 8-bit PCM playback via ISA DMA.
 *
 * The card lives at the legacy ISA base 0x220, IRQ 5, 8-bit DMA channel 1
 * (QEMU's defaults for `-device sb16`). sb16_init() probes for it with a DSP
 * reset and is a no-op if absent, so booting without the device is harmless.
 */
#ifndef OWOS_SB16_H
#define OWOS_SB16_H

void sb16_init(void);          /* probe + claim IRQ 5 (silent if no card)     */
int  sb16_present(void);       /* 1 if a card was detected                     */

/* Master-volume control, driven by the keyboard's media keys (Fn / volume
 * keys). Levels run 0..SB_VOL_MAX; the SB16 mixer attenuates the analog output
 * so a change takes effect immediately, even mid-playback. Safe to call from an
 * IRQ (they only poke the mixer ports). No-ops if no card is present. */
#define SB_VOL_MAX 8
void sb16_volume_step(int dir);   /* +1 = louder, -1 = quieter (clamped)        */
void sb16_volume_mute(void);      /* toggle mute (restores the prior level)      */
int  sb16_volume(void);           /* 0..SB_VOL_MAX, or 0 while muted             */

/* Play a RIFF/WAVE file from owofs. Any PCM 8/16-bit, mono/stereo WAV is
 * down-converted to 8-bit unsigned mono and streamed to the DAC. Ctrl+C stops
 * playback. Returns 0 on success, 1 if interrupted, -1 on error. */
int  sb16_play_wav(const char *filename);

#endif /* OWOS_SB16_H */
