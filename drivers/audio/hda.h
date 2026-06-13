/* hda.h — Intel High Definition Audio (HDA / "Azalia") PCM playback.
 *
 * Drives an HDA *controller* (PCI class 0x04 / subclass 0x03) and the audio
 * codec hanging off its serial link. On real laptops/desktops that codec is
 * almost always a Realtek ALC2xx/3xx part — so "Realtek audio" is really an HDA
 * controller + a Realtek codec, and this one driver covers both QEMU's
 * `-device intel-hda -device hda-output` and real Realtek hardware.
 *
 * hda_init() probes PCI, brings up the controller (CORB/RIRB command rings),
 * walks the codec's widget graph to find a DAC -> output-pin path, and is a
 * silent no-op if no controller is present, so booting without one is harmless.
 * It runs before interrupts are enabled, so bring-up busy-waits (like sd.c).
 */
#ifndef OWOS_HDA_H
#define OWOS_HDA_H

void hda_init(void);          /* probe + bring up the controller/codec        */
int  hda_present(void);       /* 1 if a controller + usable codec was found   */

/* Play a RIFF/WAVE file from owofs. Any PCM 8/16-bit, mono/stereo WAV is
 * resampled to 48 kHz 16-bit stereo and streamed to the codec by polling the
 * stream position register (no IRQ). Ctrl+C stops playback. Returns 0 on
 * success, 1 if interrupted, -1 on error. */
int  hda_play_wav(const char *filename);

#endif /* OWOS_HDA_H */
