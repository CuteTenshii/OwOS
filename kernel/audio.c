/* audio.c: PCM output card registry and master-volume routing. */

#include "audio.h"
#include "bt.h"
#include "es1371.h"
#include "hda.h"
#include "sb16.h"

/* Priority order: the first present card plays the stream and owns the volume keys. */
static const struct audio_card cards[] = {
    {bt_a2dp_ready, 0, 0, 0, 0, bt_a2dp_open, bt_a2dp_write, bt_a2dp_close},
    {hda_present, hda_volume_step, hda_volume_mute, hda_volume_level, hda_is_muted, hda_stream_open, hda_stream_write,
     hda_stream_close},
    {es1371_present, es1371_volume_step, es1371_volume_mute, es1371_volume_level, es1371_is_muted, es1371_stream_open,
     es1371_stream_write, es1371_stream_close},
    {sb16_present, sb16_volume_step, sb16_volume_mute, sb16_volume_level, sb16_is_muted, sb16_stream_open,
     sb16_stream_write, sb16_stream_close},
};

const struct audio_card *audio_stream_card(void)
{
  for (unsigned i = 0; i < sizeof cards / sizeof cards[0]; i++)
    if (cards[i].present())
      return &cards[i];
  return 0;
}

static const struct audio_card *volume_card(void)
{
  for (unsigned i = 0; i < sizeof cards / sizeof cards[0]; i++)
    if (cards[i].volume_step && cards[i].present())
      return &cards[i];
  return 0;
}

bool audio_volume_present(void) { return volume_card() != 0; }

void audio_volume_step(int dir)
{
  const struct audio_card *c = volume_card();
  if (c)
    c->volume_step(dir);
}

void audio_volume_mute(void)
{
  const struct audio_card *c = volume_card();
  if (c)
    c->volume_mute();
}

int audio_volume_level(void)
{
  const struct audio_card *c = volume_card();
  return c ? c->volume_level() : 0;
}

bool audio_volume_muted(void)
{
  const struct audio_card *c = volume_card();
  return c && c->is_muted();
}

void audio_volume_set(int level)
{
  for (int d = level - audio_volume_level(); d; d += d < 0 ? 1 : -1)
    audio_volume_step(d < 0 ? -1 : 1);
}
