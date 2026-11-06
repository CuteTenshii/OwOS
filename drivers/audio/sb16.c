/* sb16.c: Sound Blaster 16 PCM playback via 8237 ISA DMA (ch 1) and IRQ 5. */

#include "sb16.h"
#include "console.h"
#include "endian.h"
#include "io.h"
#include "irq.h"
#include "irqflags.h"
#include "keyboard.h"
#include "timer.h"
#include <stdbool.h>
#include <stdint.h>

/* DSP / mixer ports (offsets from the 0x220 base). */
#define SB_BASE 0x220
#define SB_MIX_ADDR (SB_BASE + 0x4)
#define SB_MIX_DATA (SB_BASE + 0x5)
#define SB_RESET (SB_BASE + 0x6)
#define SB_READ (SB_BASE + 0xA)
#define SB_WRITE (SB_BASE + 0xC) /* write cmd/data; read bit7 = busy      */
#define SB_RSTAT (SB_BASE + 0xE) /* read bit7 = data ready; read acks IRQ */

#define SB_IRQ 5
#define SB_DMA 1 /* 8-bit DMA channel                     */

/* 8237 DMA controller ports for the 8-bit channels. */
#define DMA_MASK 0x0A     /* single-channel mask register          */
#define DMA_MODE 0x0B     /* mode register                         */
#define DMA_CLEAR_FF 0x0C /* clear the low/high byte flip-flop      */
#define DMA1_ADDR 0x02    /* channel 1 base address                */
#define DMA1_COUNT 0x03   /* channel 1 count                       */
#define DMA1_PAGE 0x83    /* channel 1 page (address bits 16-23)    */

/* Two-half auto-init DMA buffer in the low 16 MiB, size-aligned so it never crosses 64 KiB. */
#define HALF 8192
#define DMABUF (2 * HALF)
static uint8_t dma_buf[DMABUF] __attribute__((aligned(DMABUF)));

static bool present;
static volatile int irq_count; /* bumped by the IRQ each time a half ends  */

/* Master volume 0..SB_VOL_MAX plus mute latch, driven by the keyboard volume keys. */
static int volume = SB_VOL_MAX;
static bool muted;

static void dsp_write(uint8_t v)
{
  /* Bounded wait: a wedged DSP never clears bit7; on expiry mark the card gone. */
  for (int tries = 0; tries < 100000; tries++) {
    if (!(inb(SB_WRITE) & 0x80)) { /* write buffer ready               */
      outb(SB_WRITE, v);
      return;
    }
  }
  present = false;
}

static void mixer_write(uint8_t reg, uint8_t v)
{
  /* Index/data is a two-port latch: mask IRQs so a volume-key write can't clobber the index. */
  uintptr_t flags = irq_save();
  outb(SB_MIX_ADDR, reg);
  outb(SB_MIX_DATA, v);
  irq_restore(flags);
}

/* Master (0x22) and voice (0x04) regs pack 4-bit L/R levels; map 0..SB_VOL_MAX onto 0..15. */
static void set_mixer_volume(void)
{
  int v = muted ? 0 : volume;
  uint8_t lvl = (uint8_t)(v * 15 / SB_VOL_MAX);
  uint8_t both = (uint8_t)((lvl << 4) | lvl);
  mixer_write(0x22, both); /* master volume                            */
  mixer_write(0x04, both); /* voice (DAC) volume                       */
}

void sb16_volume_step(int dir)
{
  int nv = volume + dir;
  if (nv < 0)
    nv = 0;
  if (nv > SB_VOL_MAX)
    nv = SB_VOL_MAX;
  volume = nv;
  muted = false; /* nudging the level always unmutes         */
  if (present)
    set_mixer_volume();
}

void sb16_volume_mute(void)
{
  muted = !muted;
  if (present)
    set_mixer_volume();
}

int sb16_volume(void) { return muted ? 0 : volume; }
int sb16_volume_level(void) { return volume; }
int sb16_is_muted(void) { return muted; }

static bool dsp_reset(void)
{
  outb(SB_RESET, 1);
  /* Hold RESET high >= 3 us; no PIT yet, so pace with io_wait (~1 us each). */
  for (int i = 0; i < 10; i++)
    io_wait();
  outb(SB_RESET, 0);
  /* Read until the 0xAA ready marker or the budget expires; other bytes may come first. */
  for (int tries = 0; tries < 100000; tries++)
    if (inb(SB_RSTAT) & 0x80)
      if (inb(SB_READ) == 0xAA)
        return true;
  return false;
}

static void sb16_irq(struct registers *r)
{
  (void)r;
  inb(SB_RSTAT); /* acknowledge the 8-bit DMA block IRQ       */
  irq_count++;
}

void sb16_init(void)
{
  if (!dsp_reset()) {
    present = false;
    con_write("sb16: no device (run QEMU with -device sb16)\n");
    return;
  }
  present = true;
  irq_register(SB_IRQ, sb16_irq);
  irq_unmask(SB_IRQ);
  set_mixer_volume(); /* defaults to SB_VOL_MAX (== 0xFF L+R)     */
  con_write("sb16: Sound Blaster 16 ready (io 0x220 irq 5 dma 1)\n");
}

int sb16_present(void) { return present; }

/* Program 8237 ch 1 for auto-init over the whole buffer so the card streams continuously. */
static void dma_setup_autoinit(uint32_t phys, uint32_t count)
{
  uint16_t off = phys & 0xFFFF;
  uint8_t page = (phys >> 16) & 0xFF;

  outb(DMA_MASK, 0x04 | SB_DMA); /* mask channel 1 while we set up   */
  outb(DMA_CLEAR_FF, 0xFF);
  outb(DMA_MODE, 0x58 | SB_DMA); /* single, auto-init, read, ch 1    */
  outb(DMA1_ADDR, off & 0xFF);
  outb(DMA1_ADDR, (off >> 8) & 0xFF);
  outb(DMA1_PAGE, page);
  outb(DMA_CLEAR_FF, 0xFF);
  outb(DMA1_COUNT, (count - 1) & 0xFF);
  outb(DMA1_COUNT, ((count - 1) >> 8) & 0xFF);
  outb(DMA_MASK, SB_DMA); /* unmask -- transfer may begin     */
}

/* Streaming PCM sink fed from /bin/play: 48 kHz S16 stereo down-converted to 8-bit mono, paced by half IRQs. */
static bool s_active;  /* a stream is open                                  */
static bool s_started; /* both halves primed, DMA running                   */
static int s_half;     /* half currently being filled by the producer (0/1) */
static uint32_t s_pos; /* fill cursor within that half (mono bytes)         */

/* Kick off the continuous auto-init transfer at 48 kHz, unsigned 8-bit mono. */
static void sb_start(void)
{
  dsp_write(0xD1); /* speaker on                       */
  irq_count = 0;
  dma_setup_autoinit((uint32_t)(uintptr_t)dma_buf, DMABUF);
  dsp_write(0x41); /* set output sample rate (Hz)      */
  dsp_write((48000 >> 8) & 0xFF);
  dsp_write(48000 & 0xFF);
  dsp_write(0xC6);              /* 8-bit auto-init DAC              */
  dsp_write(0x00);              /* mode: unsigned mono              */
  dsp_write((HALF - 1) & 0xFF); /* IRQ interval = one half          */
  dsp_write(((HALF - 1) >> 8) & 0xFF);
}

/* Block until half H is no longer playing (card plays half irq_count & 1); -2 on Ctrl+C. */
static int sb_wait_half_free(int H)
{
  int stall = 0;
  while ((irq_count & 1) == H) {
    if (keyboard_take_interrupt())
      return -2;
    if (++stall > 2000) /* ~4 s: IRQ 5 lost or DSP wedged, give up */
      return -1;
    timer_sleep(2);
  }
  return 0;
}

int sb16_stream_open(void)
{
  if (!present)
    return -1;
  set_mixer_volume();
  s_active = true;
  s_started = false;
  s_half = 0;
  s_pos = 0;
  keyboard_take_interrupt(); /* drop any stale Ctrl+C            */
  return 0;
}

/* Push `bytes` of 48 kHz/S16/stereo PCM, blocking until queued. Returns bytes
 * consumed, -1 if no stream is open, or -2 if Ctrl+C was pressed. */
int sb16_stream_write(const void *pcm, uint32_t bytes)
{
  if (!s_active)
    return -1;
  const uint8_t *p = pcm;
  uint32_t frames = bytes / 4; /* 4 bytes per S16-stereo frame     */
  for (uint32_t fi = 0; fi < frames; fi++) {
    if (s_started && s_pos == 0) { /* about to (re)fill a half         */
      if (sb_wait_half_free(s_half) < 0)
        return -2;
    }
    const uint8_t *s = p + fi * 4;
    int l = (int16_t)load_le16(s);
    int r = (int16_t)load_le16(s + 2);
    int m = (l + r) / 2;
    dma_buf[s_half * HALF + s_pos] = (uint8_t)((m >> 8) + 128);
    if (++s_pos == HALF) {
      s_pos = 0;
      if (!s_started) {
        if (s_half == 0) {
          s_half = 1; /* prime the second half            */
        } else {
          sb_start(); /* both halves primed -> go         */
          s_started = true;
          s_half = 0; /* refill half 0 once it's played   */
        }
      } else {
        s_half ^= 1;
      }
    }
  }
  return (int)(frames * 4);
}

void sb16_stream_close(void)
{
  if (!s_active)
    return;

  if (!s_started) {
    /* Clip shorter than two halves: pad with silence and play it once. */
    uint32_t real = (uint32_t)s_half * HALF + s_pos;
    for (uint32_t i = s_pos; i < HALF; i++)
      dma_buf[s_half * HALF + i] = 128;
    if (s_half == 0)
      for (uint32_t i = 0; i < HALF; i++)
        dma_buf[HALF + i] = 128;
    sb_start();
    s_started = true;
    timer_sleep(real / 48 + 40); /* 48 bytes/ms at 48 kHz mono + tail */
  } else {
    /* Follow the last real audio with two halves of silence, then drain. */
    for (int k = 0; k < 2; k++) {
      if (sb_wait_half_free(s_half) < 0)
        break;
      for (uint32_t i = 0; i < HALF; i++)
        dma_buf[s_half * HALF + i] = 128;
      s_half ^= 1;
    }
    timer_sleep(HALF / 48 + 40);
  }

  dsp_write(0xD0); /* halt the DMA                     */
  dsp_write(0xD3); /* speaker off                      */
  s_active = false;
  s_started = false;
}
