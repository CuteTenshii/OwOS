/* es1371.c: Ensoniq ES1371/ES1373 driver; polled cyclic-DMA stereo PCM out on DAC2 via the AC'97 codec. */

#include "es1371.h"
#include "console.h"
#include "io.h"
#include "keyboard.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "timer.h"
#include <stdbool.h>
#include <stdint.h>

#define R_CONTROL 0x00
#define R_STATUS 0x04
#define R_MEM_PAGE 0x0C
#define R_SRC 0x10
#define R_CODEC 0x14
#define R_LEGACY 0x18
#define R_SERIAL 0x20
#define R_DAC2_COUNT 0x28
#define R_DAC2_FRAME 0x38 /* in page PAGE_DAC */
#define R_DAC2_SIZE 0x3C  /* in page PAGE_DAC: low 16 = dwords-1, high 16 = current dword */
#define PAGE_DAC 0x0C

#define CTRL_DAC2_EN (1u << 5)
#define CTRL_SYNC_RES (1u << 14)

#define SRC_RAM_WE (1u << 24)
#define SRC_RAM_BUSY (1u << 23)
#define SRC_DISABLE (1u << 22)
#define SRC_DIS_P1 (1u << 21)
#define SRC_DIS_P2 (1u << 20)
#define SRC_DIS_R1 (1u << 19)
#define SRC_ADDR(a) ((uint32_t)(a) << 25)

#define CODEC_RDY (1u << 31)
#define CODEC_WIP (1u << 30)
#define CODEC_READ (1u << 23)

#define SER_P2_END_INC(o) (((uint32_t)(o) & 7) << 19)
#define SER_P2_ST_INC(o) (((uint32_t)(o) & 7) << 16)
#define SER_P2_PAUSE (1u << 12)
#define SER_P2_INT_EN (1u << 9)
#define SER_P2_MODE(o) (((uint32_t)(o) & 3) << 2)
#define SER_P2_MODE_S16_STEREO 3

/* Sample rate converter RAM: per-channel register blocks. */
#define SMP_DAC2 0x74
#define SMP_VOL_ADC 0x6C
#define SMP_VOL_DAC1 0x7C
#define SMP_VOL_DAC2 0x7E
#define SMP_TRUNC_N 0x00
#define SMP_INT_REGS 0x01
#define SMP_VFREQ_FRAC 0x03

/* AC'97 registers. */
#define AC_MASTER 0x02
#define AC_HEADPHONE 0x04
#define AC_PCM_OUT 0x18
#define AC_POWERDOWN 0x26
#define AC_MUTE 0x8000

#define AUDIO_BYTES 0x20000u /* 128 KiB cyclic buffer, ~680 ms at 48k/S16 stereo */
#define STREAM_GUARD 256u    /* bytes kept clear ahead of the play head          */

static uint16_t io;
static bool present;
static uint32_t ctrl, sctrl;
static uint8_t *audio;
static uint32_t audio_phys;

static int volume = ES_VOL_MAX;
static bool muted;

static bool s_active;
static bool s_started;
static uint32_t s_wpos;

static void spin(int n)
{
  while (n-- > 0)
    io_wait();
}

static uint32_t src_wait(void)
{
  uint32_t r = 0;
  for (int t = 0; t < 0x1000; t++) {
    r = inl(io + R_SRC);
    if (!(r & SRC_RAM_BUSY))
      break;
    spin(1);
  }
  return r;
}

static void src_write(uint32_t reg, uint32_t data)
{
  uint32_t r = src_wait() & (SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_P2 | SRC_DIS_R1);
  outl(io + R_SRC, r | SRC_ADDR(reg) | (data & 0xFFFF) | SRC_RAM_WE);
}

/* DAC2 at 48 kHz: the converter runs at unity ratio (integer part 16, no fraction). */
static void src_init(void)
{
  for (uint32_t i = 0; i < 0x80; i++)
    src_write(i, 0);
  src_write(SMP_DAC2 + SMP_TRUNC_N, 16 << 4);
  src_write(SMP_DAC2 + SMP_INT_REGS, 16 << 10);
  src_write(SMP_DAC2 + SMP_VFREQ_FRAC, 0);
  src_write(SMP_VOL_ADC, 1 << 12);
  src_write(SMP_VOL_ADC + 1, 1 << 12);
  src_write(SMP_VOL_DAC1, 1 << 12);
  src_write(SMP_VOL_DAC1 + 1, 1 << 12);
  src_write(SMP_VOL_DAC2, 1 << 12);
  src_write(SMP_VOL_DAC2 + 1, 1 << 12);
  src_wait();
  outl(io + R_SRC, 0);
}

static void codec_write(uint8_t reg, uint16_t val)
{
  src_wait();
  for (int t = 0; t < 0x1000; t++) {
    if (!(inl(io + R_CODEC) & CODEC_WIP)) {
      outl(io + R_CODEC, ((uint32_t)reg << 16) | val);
      return;
    }
    spin(1);
  }
}

static uint16_t codec_read(uint8_t reg)
{
  src_wait();
  for (int t = 0; t < 0x1000 && (inl(io + R_CODEC) & CODEC_WIP); t++)
    spin(1);
  outl(io + R_CODEC, ((uint32_t)reg << 16) | CODEC_READ);
  for (int t = 0; t < 0x1000 && (inl(io + R_CODEC) & CODEC_WIP); t++)
    spin(1);
  for (int t = 0; t < 0x1000; t++) {
    uint32_t v = inl(io + R_CODEC);
    if (v & CODEC_RDY)
      return (uint16_t)v;
    spin(1);
  }
  return 0;
}

/* Master and headphone attenuation are 5 bits per channel (0 = loudest). */
static void apply_volume(void)
{
  uint32_t att = (uint32_t)(ES_VOL_MAX - volume) * 31 / ES_VOL_MAX;
  uint16_t v = muted ? AC_MUTE : (uint16_t)((att << 8) | att);
  codec_write(AC_MASTER, v);
  codec_write(AC_HEADPHONE, v);
}

void es1371_volume_step(int dir)
{
  int nv = volume + dir;
  volume = nv < 0 ? 0 : nv > ES_VOL_MAX ? ES_VOL_MAX : nv;
  muted = false;
  if (present)
    apply_volume();
}

void es1371_volume_mute(void)
{
  muted = !muted;
  if (present)
    apply_volume();
}

int es1371_volume(void) { return muted ? 0 : volume; }
int es1371_volume_level(void) { return volume; }
int es1371_is_muted(void) { return muted; }
bool es1371_present(void) { return present; }

static bool is_es1371(uint32_t id) { return id == 0x13711274u || id == 0x58801274u || id == 0x89381102u; }

static bool find_card(uint8_t *bus, uint8_t *slot, uint8_t *func)
{
  for (int b = 0; b < 256; b++)
    for (int s = 0; s < 32; s++) {
      if ((pci_read32((uint8_t)b, (uint8_t)s, 0, 0x00) & 0xFFFF) == 0xFFFF)
        continue;
      for (int f = 0; f < 8; f++)
        if (is_es1371(pci_read32((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00))) {
          *bus = (uint8_t)b;
          *slot = (uint8_t)s;
          *func = (uint8_t)f;
          return true;
        }
    }
  return false;
}

void es1371_init(void)
{
  uint8_t bus, slot, func;
  if (!find_card(&bus, &slot, &func))
    return;

  uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
  if (!(bar0 & 1) || !(bar0 & 0xFFFC)) {
    con_write("es1371: card has no I/O base\n");
    return;
  }
  io = (uint16_t)(bar0 & 0xFFFC);
  /* I/O-space decode (bit0) + bus-master DMA (bit2). */
  pci_write32(bus, slot, func, 0x04, pci_read32(bus, slot, func, 0x04) | 0x05);

  uint32_t base = pmm_alloc_contig(AUDIO_BYTES);
  if (!base || paging_identity_map(base, AUDIO_BYTES) != 0) {
    con_write("es1371: DMA buffer alloc failed\n");
    return;
  }
  audio = (uint8_t *)(uintptr_t)base;
  audio_phys = base;

  ctrl = 0;
  sctrl = 0;
  outl(io + R_CONTROL, CTRL_SYNC_RES);
  inl(io + R_CONTROL);
  spin(20);
  outl(io + R_CONTROL, ctrl);
  outl(io + R_SERIAL, sctrl);
  outl(io + R_LEGACY, 0);
  src_init();

  /* Wait for the codec to report all sections powered up (low nibble 0xF). */
  bool up = false;
  for (int t = 0; t < 100 && !up; t++) {
    up = (codec_read(AC_POWERDOWN) & 0x0F) == 0x0F;
    if (!up)
      timer_sleep(10);
  }
  if (!up) {
    con_write("es1371: AC'97 codec not ready\n");
    return;
  }

  codec_write(AC_PCM_OUT, 0x0808);
  apply_volume();
  present = true;
  con_printf("es1371: Ensoniq AudioPCI ready (io 0x%x)\n", io);
}

int es1371_stream_open(void)
{
  if (!present)
    return -1;
  ctrl &= ~CTRL_DAC2_EN;
  outl(io + R_CONTROL, ctrl);
  for (uint32_t i = 0; i < AUDIO_BYTES; i++)
    audio[i] = 0;

  sctrl &= ~(SER_P2_PAUSE | SER_P2_INT_EN | SER_P2_MODE(3) | SER_P2_ST_INC(7) | SER_P2_END_INC(7));
  sctrl |= SER_P2_MODE(SER_P2_MODE_S16_STEREO) | SER_P2_ST_INC(0) | SER_P2_END_INC(2);
  outl(io + R_SERIAL, sctrl);
  outl(io + R_MEM_PAGE, PAGE_DAC);
  outl(io + R_DAC2_FRAME, audio_phys);
  outl(io + R_DAC2_SIZE, AUDIO_BYTES / 4 - 1);
  outl(io + R_DAC2_COUNT, AUDIO_BYTES / 4 - 1);

  s_active = true;
  s_started = false;
  s_wpos = 0;
  keyboard_take_interrupt();
  return 0;
}

static uint32_t play_pos(void)
{
  outl(io + R_MEM_PAGE, PAGE_DAC);
  return (inl(io + R_DAC2_SIZE) >> 14) & 0x3FFFC;
}

static void start_dma(void)
{
  __asm__ volatile("" ::: "memory");
  ctrl |= CTRL_DAC2_EN;
  outl(io + R_CONTROL, ctrl);
  s_started = true;
  s_wpos = 0;
}

int es1371_stream_write(const void *pcm, uint32_t bytes)
{
  if (!s_active)
    return -1;
  const uint8_t *p = pcm;
  uint32_t off = 0;
  uint32_t last_pos = 0;
  int stall = 0;
  while (off < bytes) {
    if (keyboard_take_interrupt())
      return -2;

    if (!s_started) {
      uint32_t n = bytes - off;
      if (n > AUDIO_BYTES - s_wpos)
        n = AUDIO_BYTES - s_wpos;
      for (uint32_t i = 0; i < n; i++)
        audio[s_wpos + i] = p[off + i];
      s_wpos += n;
      off += n;
      if (s_wpos >= AUDIO_BYTES)
        start_dma();
      continue;
    }

    uint32_t rpos = play_pos();
    if (rpos != last_pos) {
      last_pos = rpos;
      stall = 0;
    }
    uint32_t freebytes = (rpos + AUDIO_BYTES - s_wpos) % AUDIO_BYTES;
    uint32_t can = freebytes > STREAM_GUARD ? (freebytes - STREAM_GUARD) & ~3u : 0;
    if (can == 0) {
      if (++stall > 2000)
        return off ? (int)off : -1;
      timer_sleep(2);
      continue;
    }
    uint32_t n = bytes - off;
    if (n > can)
      n = can;
    if (n > AUDIO_BYTES - s_wpos)
      n = AUDIO_BYTES - s_wpos;
    for (uint32_t i = 0; i < n; i++)
      audio[s_wpos + i] = p[off + i];
    s_wpos = (s_wpos + n) % AUDIO_BYTES;
    off += n;
  }
  return (int)off;
}

void es1371_stream_close(void)
{
  if (!s_active)
    return;

  if (!s_started) {
    uint32_t real = s_wpos;
    for (uint32_t i = s_wpos; i < AUDIO_BYTES; i++)
      audio[i] = 0;
    start_dma();
    timer_sleep(real / 192 + 30);
  } else {
    /* A full buffer of silence proves the DMA consumed every real sample. */
    uint32_t silence = AUDIO_BYTES;
    int guard = 0;
    while (silence > 0 && guard < 4000) {
      if (keyboard_take_interrupt())
        break;
      uint32_t rpos = play_pos();
      uint32_t freebytes = (rpos + AUDIO_BYTES - s_wpos) % AUDIO_BYTES;
      uint32_t can = freebytes > STREAM_GUARD ? (freebytes - STREAM_GUARD) & ~3u : 0;
      if (can == 0) {
        timer_sleep(2);
        guard++;
        continue;
      }
      uint32_t n = silence;
      if (n > can)
        n = can;
      if (n > AUDIO_BYTES - s_wpos)
        n = AUDIO_BYTES - s_wpos;
      for (uint32_t i = 0; i < n; i++)
        audio[s_wpos + i] = 0;
      s_wpos = (s_wpos + n) % AUDIO_BYTES;
      silence -= n;
    }
    timer_sleep(180);
  }

  ctrl &= ~CTRL_DAC2_EN;
  outl(io + R_CONTROL, ctrl);
  s_active = false;
  s_started = false;
}
