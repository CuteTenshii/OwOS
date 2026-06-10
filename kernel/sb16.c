/* sb16.c — Sound Blaster 16 PCM playback.
 *
 * Bring-up follows the classic SB16 sequence: reset the DSP (expect 0xAA),
 * unmute the mixer, then for each audio block program the 8237 ISA DMA
 * controller (channel 1) to point at our buffer, set the output rate, and kick
 * off a single-cycle 8-bit transfer. The card raises IRQ 5 when the block
 * finishes; we wait for it, refill the buffer, and repeat. Single-cycle (vs
 * auto-init double-buffering) keeps the logic simple at the cost of a tiny gap
 * between ~0.4 s blocks.
 */

#include <stdint.h>
#include "sb16.h"
#include "io.h"
#include "irq.h"
#include "vga.h"
#include "fs.h"
#include "keyboard.h"

/* DSP / mixer ports (offsets from the 0x220 base). */
#define SB_BASE     0x220
#define SB_MIX_ADDR (SB_BASE + 0x4)
#define SB_MIX_DATA (SB_BASE + 0x5)
#define SB_RESET    (SB_BASE + 0x6)
#define SB_READ     (SB_BASE + 0xA)
#define SB_WRITE    (SB_BASE + 0xC)   /* write cmd/data; read bit7 = busy      */
#define SB_RSTAT    (SB_BASE + 0xE)   /* read bit7 = data ready; read acks IRQ */

#define SB_IRQ      5
#define SB_DMA      1                 /* 8-bit DMA channel                     */

/* 8237 DMA controller ports for the 8-bit channels. */
#define DMA_MASK     0x0A             /* single-channel mask register          */
#define DMA_MODE     0x0B             /* mode register                         */
#define DMA_CLEAR_FF 0x0C             /* clear the low/high byte flip-flop      */
#define DMA1_ADDR    0x02             /* channel 1 base address                */
#define DMA1_COUNT   0x03             /* channel 1 count                       */
#define DMA1_PAGE    0x83             /* channel 1 page (address bits 16-23)    */

/* The DMA buffer must sit in the low 16 MiB (it does -- the kernel image is
 * identity-mapped there) and must not straddle a 64 KiB physical boundary. An
 * N-byte buffer aligned to N (with N dividing 64 KiB) never can. */
/* One auto-init DMA buffer split into two halves: the card plays one half
 * while we refill the other, so the DAC never stops between blocks (gapless).
 * The whole buffer is aligned to its own size so it can't straddle a 64 KiB
 * DMA boundary. */
#define HALF    8192
#define DMABUF  (2 * HALF)
static uint8_t dma_buf[DMABUF] __attribute__((aligned(DMABUF)));

static int present;
static volatile int irq_count;     /* bumped by the IRQ each time a half ends  */

/* ---- DSP / mixer primitives -------------------------------------------- */

static void dsp_write(uint8_t v)
{
	while (inb(SB_WRITE) & 0x80)           /* wait for the write buffer        */
		;
	outb(SB_WRITE, v);
}

static void mixer_write(uint8_t reg, uint8_t v)
{
	outb(SB_MIX_ADDR, reg);
	outb(SB_MIX_DATA, v);
}

static int dsp_reset(void)
{
	outb(SB_RESET, 1);
	for (volatile int i = 0; i < 10000; i++)   /* hold reset >= 3 us           */
		;
	outb(SB_RESET, 0);
	for (int tries = 0; tries < 100000; tries++)
		if (inb(SB_RSTAT) & 0x80)
			return inb(SB_READ) == 0xAA;       /* DSP reports ready            */
	return 0;
}

static void sb16_irq(struct registers *r)
{
	(void)r;
	inb(SB_RSTAT);                 /* acknowledge the 8-bit DMA block IRQ       */
	irq_count++;
}

void sb16_init(void)
{
	if (!dsp_reset()) {
		present = 0;
		vga_write("sb16: no device (run QEMU with -device sb16)\n");
		return;
	}
	present = 1;
	irq_register(SB_IRQ, sb16_irq);
	irq_unmask(SB_IRQ);
	mixer_write(0x22, 0xFF);        /* master volume to max                     */
	mixer_write(0x04, 0xFF);        /* voice (DAC) volume to max                */
	vga_write("sb16: Sound Blaster 16 ready (io 0x220 irq 5 dma 1)\n");
}

int sb16_present(void)
{
	return present;
}

/* ---- DMA + gapless playback -------------------------------------------- */

/* Program the 8237 controller (channel 1) for an auto-init transfer over the
 * whole buffer: when it reaches the end it reloads and keeps going, so the
 * card streams continuously instead of stopping after each block. */
static void dma_setup_autoinit(uint32_t phys, uint32_t count)
{
	uint16_t off  = phys & 0xFFFF;
	uint8_t  page = (phys >> 16) & 0xFF;

	outb(DMA_MASK, 0x04 | SB_DMA);          /* mask channel 1 while we set up   */
	outb(DMA_CLEAR_FF, 0xFF);
	outb(DMA_MODE, 0x58 | SB_DMA);          /* single, auto-init, read, ch 1    */
	outb(DMA1_ADDR, off & 0xFF);
	outb(DMA1_ADDR, (off >> 8) & 0xFF);
	outb(DMA1_PAGE, page);
	outb(DMA_CLEAR_FF, 0xFF);
	outb(DMA1_COUNT, (count - 1) & 0xFF);
	outb(DMA1_COUNT, ((count - 1) >> 8) & 0xFF);
	outb(DMA_MASK, SB_DMA);                 /* unmask -- transfer may begin     */
}

/* ---- WAV parsing + streaming ------------------------------------------- */

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static int tag_eq(const uint8_t *p, const char *s)
{
	return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3];
}

/* Fill one HALF-byte buffer with the next slice of audio, down-converting each
 * frame to one 8-bit unsigned mono sample and padding any shortfall at EOF with
 * silence (0x80). Advances *consumed. Returns the count of real audio samples. */
static uint32_t fill_half(uint8_t *dst, const char *fn, uint32_t data_off,
                          uint32_t data_len, uint32_t *consumed,
                          uint16_t bits, uint16_t channels, uint32_t frame)
{
	static uint8_t in[HALF * 4];           /* worst case: 16-bit stereo input  */
	uint32_t produced = 0;

	if (*consumed < data_len) {
		uint32_t in_bytes = HALF * frame;
		if (*consumed + in_bytes > data_len)
			in_bytes = data_len - *consumed;
		int r = fs_read_at(fn, data_off + *consumed, in, in_bytes);
		if (r > 0) {
			uint32_t frames = (uint32_t)r / frame;
			for (uint32_t i = 0; i < frames; i++) {
				const uint8_t *s = in + i * frame;
				int v;
				if (bits == 8) {
					v = (channels == 2) ? ((int)s[0] + s[1]) / 2 : s[0];
				} else {
					int v0 = (int16_t)rd16(s);
					if (channels == 2)
						v0 = (v0 + (int16_t)rd16(s + 2)) / 2;
					v = (v0 >> 8) + 128;
				}
				dst[i] = (uint8_t)v;
			}
			produced = frames;
			*consumed += frames * frame;
		}
	}
	for (uint32_t i = produced; i < HALF; i++)     /* pad the rest with silence */
		dst[i] = 128;
	return produced;
}

int sb16_play_wav(const char *fn)
{
	if (!present) {
		vga_write("play: no sound card (start QEMU with -device sb16)\n");
		return -1;
	}

	int fsize = fs_size(fn);
	if (fsize < 0) {
		vga_write("play: no such file: ");
		vga_write(fn);
		vga_putc('\n');
		return -1;
	}

	uint8_t hdr[256];
	int got = fs_read_at(fn, 0, hdr, sizeof(hdr));
	if (got < 44 || !tag_eq(hdr, "RIFF") || !tag_eq(hdr + 8, "WAVE")) {
		vga_write("play: not a WAV file\n");
		return -1;
	}

	/* Walk the chunk list for "fmt " and "data" (other chunks may sit between
	 * them). Both normally live within the first few dozen bytes. */
	uint16_t fmt = 0, channels = 0, bits = 0;
	uint32_t rate = 0, data_off = 0, data_len = 0;
	for (uint32_t pos = 12; pos + 8 <= (uint32_t)got; ) {
		const uint8_t *c = hdr + pos;
		uint32_t sz = rd32(c + 4);
		if (tag_eq(c, "fmt ")) {
			fmt      = rd16(c + 8);
			channels = rd16(c + 10);
			rate     = rd32(c + 12);
			bits     = rd16(c + 22);
		} else if (tag_eq(c, "data")) {
			data_off = pos + 8;
			data_len = sz;
			break;
		}
		pos += 8 + sz + (sz & 1);       /* chunks are word-aligned              */
	}

	if (!data_off || fmt != 1 || (bits != 8 && bits != 16) ||
	    channels < 1 || channels > 2) {
		vga_write("play: unsupported WAV (need PCM 8/16-bit mono/stereo)\n");
		return -1;
	}
	if (data_off + data_len > (uint32_t)fsize)      /* clamp to the real file   */
		data_len = (uint32_t)fsize - data_off;
	if (rate < 4000)  rate = 4000;
	if (rate > 44100) rate = 44100;

	uint32_t frame = (uint32_t)channels * (bits / 8);   /* bytes per input frame */
	uint32_t consumed = 0;

	keyboard_take_interrupt();              /* drop any stale Ctrl+C            */
	dsp_write(0xD1);                        /* speaker on                       */

	/* Prime both halves, then start a continuous auto-init transfer over the
	 * whole buffer. The DSP raises an IRQ every HALF bytes but keeps playing. */
	fill_half(dma_buf,        fn, data_off, data_len, &consumed, bits, channels, frame);
	fill_half(dma_buf + HALF, fn, data_off, data_len, &consumed, bits, channels, frame);

	irq_count = 0;
	dma_setup_autoinit((uint32_t)dma_buf, DMABUF);
	dsp_write(0x41);                        /* set output sample rate (Hz)      */
	dsp_write((rate >> 8) & 0xFF);
	dsp_write(rate & 0xFF);
	dsp_write(0xC6);                        /* 8-bit auto-init DAC              */
	dsp_write(0x00);                        /* mode: unsigned mono              */
	dsp_write((HALF - 1) & 0xFF);           /* IRQ interval = one half          */
	dsp_write(((HALF - 1) >> 8) & 0xFF);

	/* On each block IRQ, refill the half that just finished (the one the card
	 * is no longer reading) while the other half keeps playing -- so the DAC
	 * never starves. After the stream ends, drain two more halves of silence. */
	int interrupted = 0, last = 0, next = 0, drain = 0;
	for (;;) {
		while (irq_count == last) {
			if (keyboard_take_interrupt()) {
				interrupted = 1;
				break;
			}
			__asm__ volatile ("hlt");
		}
		if (interrupted)
			break;
		last = irq_count;

		if (consumed < data_len) {
			fill_half(dma_buf + next * HALF, fn, data_off, data_len,
			          &consumed, bits, channels, frame);
		} else {
			if (drain >= 2)
				break;
			for (uint32_t i = 0; i < HALF; i++)
				dma_buf[next * HALF + i] = 128;     /* trailing silence */
			drain++;
		}
		next ^= 1;
	}

	dsp_write(0xD0);                        /* halt the DMA                     */
	dsp_write(0xD3);                        /* speaker off                      */
	return interrupted ? 1 : 0;
}
