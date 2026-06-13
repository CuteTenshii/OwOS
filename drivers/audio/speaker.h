/* speaker.h — PC speaker tones via PIT channel 2.
 *
 * Channel 2 is wired to the speaker; gating it through port 0x61 produces a
 * square wave at whatever frequency the channel is programmed to. This is
 * independent of channel 0, which drives the system timer.
 */
#ifndef OWOS_SPEAKER_H
#define OWOS_SPEAKER_H

#include <stdint.h>

void speaker_tone(uint32_t freq);          /* start a continuous tone        */
void speaker_off(void);                    /* silence                        */
void speaker_beep(uint32_t freq, uint32_t ms);
void speaker_song(void);                   /* play the default tune (twinkle) */

/* Play a named tune ("twinkle", "megalovania", "sweden"). Returns 0 if found,
 * -1 if the name matches no tune. */
int  speaker_play(const char *name);

#endif /* OWOS_SPEAKER_H */
