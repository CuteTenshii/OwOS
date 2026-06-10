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

/* Play a RIFF/WAVE file from owofs. Any PCM 8/16-bit, mono/stereo WAV is
 * down-converted to 8-bit unsigned mono and streamed to the DAC. Ctrl+C stops
 * playback. Returns 0 on success, 1 if interrupted, -1 on error. */
int  sb16_play_wav(const char *filename);

#endif /* OWOS_SB16_H */
