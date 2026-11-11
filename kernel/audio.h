/* audio.h: registry of PCM output cards. A driver joins by adding one entry to cards[] in audio.c. */
#ifndef OWOS_AUDIO_H
#define OWOS_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

struct audio_card {
  bool (*present)(void);
  /* Volume ops may be NULL (Bluetooth sinks control their own volume). */
  void (*volume_step)(int dir);
  void (*volume_mute)(void);
  int (*volume_level)(void); /* 0..AUDIO_VOL_MAX, ignoring mute */
  int (*is_muted)(void);
  /* Streaming sink for 48 kHz/S16/stereo PCM; write() returns bytes consumed, -1 no stream, -2 Ctrl+C. */
  int (*stream_open)(void);
  int (*stream_write)(const void *pcm, uint32_t bytes);
  void (*stream_close)(void);
};

#define AUDIO_VOL_MAX 8

/* First present card in priority order, or NULL. */
const struct audio_card *audio_stream_card(void);

/* Master volume on the first present card that has volume controls; no-ops/zero without one. */
bool audio_volume_present(void);
void audio_volume_step(int dir);
void audio_volume_mute(void);
int audio_volume_level(void);
bool audio_volume_muted(void);
/* Step to an absolute level (clamped to 0..AUDIO_VOL_MAX). */
void audio_volume_set(int level);

#endif /* OWOS_AUDIO_H */
