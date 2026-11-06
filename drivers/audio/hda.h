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

#include <stdint.h>

void hda_init(void);   /* probe + bring up the controller/codec        */
int hda_present(void); /* 1 if a controller + usable codec was found   */

/* Master-volume control on the output path's amp, driven by the keyboard's
 * media keys — the HDA counterpart to sb16_volume_*. Same 0..HDA_VOL_MAX scale
 * so keyboard.c can treat both cards uniformly. Safe to call from the keyboard
 * IRQ (codec_cmd runs with interrupts masked). No-ops if no card / no amp. */
#define HDA_VOL_MAX 8
void hda_volume_step(int dir); /* +1 = louder, -1 = quieter (clamped)        */
void hda_volume_mute(void);    /* toggle mute (restores the prior level)      */
int hda_volume(void);          /* 0..HDA_VOL_MAX, or 0 while muted            */
int hda_volume_level(void);    /* 0..HDA_VOL_MAX, ignoring mute               */
int hda_is_muted(void);        /* 1 while muted                               */

/* Streaming PCM sink, fed from /bin/play over the SYS_AUDIO_* calls.
 * The producer pushes a fixed canonical format — 48 kHz, 16-bit, stereo — which
 * is exactly what the DMA buffer carries, so the WAV parse + resample stay in
 * user space and this is just the back end. open() configures the path and DMA;
 * write() copies frames into the cyclic buffer behind the play head, blocking
 * (back-pressure, no IRQ) until the DMA frees room — it returns the bytes
 * consumed, -1 if no stream is open, or -2 if Ctrl+C was pressed; close()
 * drains and stops. One stream at a time. */
int hda_stream_open(void);
int hda_stream_write(const void *pcm, uint32_t bytes);
void hda_stream_close(void);

/* Output endpoint selection — speaker / headphones / line-out on the codec.
 * The codec usually exposes more than one output pin; these let the Sound
 * settings panel and /sys/class/sound enumerate them and switch the active one
 * at runtime (re-routing a live stream). hda_output_select() takes an index
 * into the list. All degrade gracefully (count 0, active -1, select fails) when
 * no codec is present. */
int hda_output_count(void);                     /* number of switchable outputs   */
int hda_output_name(int i, char *out, int max); /* output i's name (1 if valid) */
int hda_output_active(void);                    /* active index, or -1            */
int hda_output_select(int i);                   /* switch active output; 0 on ok  */

#endif /* OWOS_HDA_H */
