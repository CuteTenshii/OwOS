/* speaker.c — drive the PC speaker (PIT channel 2 + port 0x61). */

#include "speaker.h"
#include "io.h"
#include "timer.h"
#include "keyboard.h"

#define PIT_CH2_DATA 0x42
#define PIT_CMD      0x43
#define SPEAKER_PORT 0x61
#define PIT_BASE_HZ  1193182
#define PIT_CH2_MODE3 0xB6   /* channel 2, lo/hi byte, square wave, binary   */

void speaker_tone(uint32_t freq)
{
	if (freq == 0) {
		speaker_off();
		return;
	}
	uint32_t divisor = PIT_BASE_HZ / freq;
	outb(PIT_CMD, PIT_CH2_MODE3);
	outb(PIT_CH2_DATA, divisor & 0xFF);
	outb(PIT_CH2_DATA, (divisor >> 8) & 0xFF);

	uint8_t v = inb(SPEAKER_PORT);
	outb(SPEAKER_PORT, v | 0x03);      /* bit0: gate ch2, bit1: speaker on    */
}

void speaker_off(void)
{
	outb(SPEAKER_PORT, inb(SPEAKER_PORT) & ~0x03);
}

void speaker_beep(uint32_t freq, uint32_t ms)
{
	speaker_tone(freq);
	timer_sleep(ms);
	speaker_off();
}

/* Note frequencies in Hz (equal temperament, A4 = 440). Sharps are named as
 * flats (Gb4 == F#4, etc). REST is silence -- speaker_tone(0) gates off. */
#define REST 0
#define B3  247
#define C4  262
#define D4  294
#define E4  330
#define F4  349
#define Gb4 370
#define G4  392
#define Ab4 415
#define A4  440
#define Bb4 466
#define B4  494
#define C5  523
#define D5  587
#define E5  659
#define F5  698
#define G5  784
#define A5  880

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

struct note { uint16_t freq; uint16_t ms; };

/* "Twinkle, Twinkle, Little Star" — recognizable and short. */
static const struct note twinkle[] = {
	{C4,400},{C4,400},{G4,400},{G4,400},{A4,400},{A4,400},{G4,800},
	{F4,400},{F4,400},{E4,400},{E4,400},{D4,400},{D4,400},{C4,800},
};

/* Megalovania (Toby Fox) — the unmistakable opening riff, monophonic in D
 * minor: "d d d' a, ab g f d f g", four bars with the C and B variations. */
static const struct note megalovania[] = {
	{D4,150},{D4,150},{D5,300},{A4,300},{REST,150},{Ab4,150},{G4,150},{F4,300},{D4,150},{F4,150},{G4,150},
	{D4,150},{D4,150},{D5,300},{A4,300},{REST,150},{Ab4,150},{G4,150},{F4,300},{D4,150},{F4,150},{G4,150},
	{C4,150},{C4,150},{D5,300},{A4,300},{REST,150},{Ab4,150},{G4,150},{F4,300},{D4,150},{F4,150},{G4,150},
	{B3,150},{B3,150},{D5,300},{A4,300},{REST,150},{Ab4,150},{G4,150},{F4,300},{D4,150},{F4,150},{G4,150},
};

/* "Sweden" (C418) — the Minecraft menu theme. The original is a soft chordal
 * pad, so this is a single-line arrangement of the melody, not note-perfect.
 * Slow and gentle, to contrast the speaker's harsh square wave. */
static const struct note sweden[] = {
	{G4,350},{E4,350},{C4,700},{E4,350},{G4,350},{C5,700},
	{B4,350},{G4,350},{A4,700},{G4,350},{E4,350},{C4,900},{REST,250},
	{C5,350},{B4,350},{A4,700},{G4,350},{E4,350},{G4,700},
	{A4,350},{B4,350},{C5,1000},
};

/* Sleep up to ms, but return 1 early if Ctrl+C is pressed. Sleeping in small
 * chunks keeps the abort latency low even on a long held note. */
static int interruptible_sleep(uint32_t ms)
{
	while (ms > 0) {
		if (keyboard_take_interrupt())
			return 1;
		uint32_t chunk = ms < 20 ? ms : 20;
		timer_sleep(chunk);
		ms -= chunk;
	}
	return 0;
}

/* Play a note array on the speaker; a 1/10 gap per note keeps repeated notes
 * from smearing together. A REST (freq 0) gates the speaker off for its ms.
 * Ctrl+C stops playback. */
static void play_tune(const struct note *t, int n)
{
	keyboard_take_interrupt();             /* drop any stale Ctrl+C            */
	for (int i = 0; i < n; i++) {
		uint32_t play = (t[i].ms * 9) / 10;
		uint32_t gap  = t[i].ms - play;
		speaker_tone(t[i].freq);
		if (interruptible_sleep(play))
			break;
		speaker_off();
		if (interruptible_sleep(gap))
			break;
	}
	speaker_off();
}

/* Match an argument (NUL- or space-terminated) against a tune name. */
static int name_is(const char *arg, const char *name)
{
	while (*name) {
		if (*arg != *name)
			return 0;
		arg++; name++;
	}
	return *arg == '\0' || *arg == ' ';
}

void speaker_song(void)
{
	play_tune(twinkle, ARRAY_LEN(twinkle));
}

int speaker_play(const char *name)
{
	if (name_is(name, "megalovania"))
		play_tune(megalovania, ARRAY_LEN(megalovania));
	else if (name_is(name, "sweden"))
		play_tune(sweden, ARRAY_LEN(sweden));
	else if (name_is(name, "twinkle"))
		play_tune(twinkle, ARRAY_LEN(twinkle));
	else
		return -1;
	return 0;
}
