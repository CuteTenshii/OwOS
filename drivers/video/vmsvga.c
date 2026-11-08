/* vmsvga.c: VMware SVGA II display adapter (see vmsvga.h).
 * BAR0 is the I/O register window, BAR1 the framebuffer, BAR2 the command FIFO. fb.c adopts the
 * framebuffer; fb_present calls our hook, which pushes an SVGA_CMD_UPDATE for the damage box. */

#include "vmsvga.h"
#include "console.h"
#include "fb.h"
#include "io.h"
#include "paging.h"
#include "pci.h"
#include "spinlock.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VMW_VENDOR 0x15AD
#define VMW_DEV_SVGA2 0x0405

#define SVGA_INDEX_PORT 0
#define SVGA_VALUE_PORT 1

/* SVGA registers (written via the index/value port pair). */
enum {
  SVGA_REG_ID = 0,
  SVGA_REG_ENABLE = 1,
  SVGA_REG_WIDTH = 2,
  SVGA_REG_HEIGHT = 3,
  SVGA_REG_MAX_WIDTH = 4,
  SVGA_REG_MAX_HEIGHT = 5,
  SVGA_REG_BITS_PER_PIXEL = 7,
  SVGA_REG_BYTES_PER_LINE = 12,
  SVGA_REG_FB_START = 13,
  SVGA_REG_FB_OFFSET = 14,
  SVGA_REG_VRAM_SIZE = 15,
  SVGA_REG_FB_SIZE = 16,
  SVGA_REG_CAPABILITIES = 17,
  SVGA_REG_MEM_START = 18, /* (deprecated) FIFO base; prefer BAR2        */
  SVGA_REG_MEM_SIZE = 19,  /* FIFO size in bytes                         */
  SVGA_REG_CONFIG_DONE = 20,
  SVGA_REG_SYNC = 21,
  SVGA_REG_BUSY = 22,
  SVGA_REG_CURSOR_ID = 24,
  SVGA_REG_CURSOR_X = 25,
  SVGA_REG_CURSOR_Y = 26,
  SVGA_REG_CURSOR_ON = 27,
};

#define SVGA_CAP_CURSOR_BYPASS 0x40u
#define SVGA_CAP_ALPHA_CURSOR 0x200u
#define SVGA_CMD_DEFINE_ALPHA_CURSOR 22

/* FIFO control words occupy the first four dwords of the FIFO. */
enum {
  SVGA_FIFO_MIN = 0,
  SVGA_FIFO_MAX = 1,
  SVGA_FIFO_NEXT_CMD = 2,
  SVGA_FIFO_STOP = 3,
};

#define SVGA_FIFO_NUM_REGS 4
#define SVGA_CMD_UPDATE 1

/* Version negotiation: offer ID_2 first and step down until one reads back. */
#define SVGA_MAGIC 0x900000u
#define SVGA_ID_2 ((SVGA_MAGIC << 8) | 2)
#define SVGA_ID_1 ((SVGA_MAGIC << 8) | 1)
#define SVGA_ID_0 ((SVGA_MAGIC << 8) | 0)

static uint16_t io_base;
static volatile uint32_t *fifo;
static bool queued; /* UPDATEs queued since the last SYNC */

static spinlock_t reg_lock; /* index/value pair must not interleave (mouse IRQ moves the cursor) */
static spinlock_t fifo_lock;
static bool hw_cursor;
static volatile int cursor_scale = 1;

static void reg_write(uint32_t reg, uint32_t val)
{
  uintptr_t f = spin_lock_irqsave(&reg_lock);
  outl(io_base + SVGA_INDEX_PORT, reg);
  outl(io_base + SVGA_VALUE_PORT, val);
  spin_unlock_irqrestore(&reg_lock, f);
}

static uint32_t reg_read(uint32_t reg)
{
  uintptr_t f = spin_lock_irqsave(&reg_lock);
  outl(io_base + SVGA_INDEX_PORT, reg);
  uint32_t v = inl(io_base + SVGA_VALUE_PORT);
  spin_unlock_irqrestore(&reg_lock, f);
  return v;
}

/* Free bytes in the ring; one slot stays unused so NEXT_CMD == STOP means empty. */
static uint32_t fifo_free(uint32_t next)
{
  uint32_t min = fifo[SVGA_FIFO_MIN], max = fifo[SVGA_FIFO_MAX], stop = fifo[SVGA_FIFO_STOP];
  uint32_t used = next >= stop ? next - stop : (max - min) - (stop - next);
  return (max - min) - used - 4;
}

/* Append n dwords as one unit (a split command would be consumed half-written). Needs n*4 < ring size. */
static void fifo_put(const uint32_t *words, uint32_t n, const uint32_t *tail, uint32_t ntail)
{
  uintptr_t f = spin_lock_irqsave(&fifo_lock);
  uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
  while (fifo_free(next) < (n + ntail) * 4) {
    reg_write(SVGA_REG_SYNC, 1);
    (void)reg_read(SVGA_REG_BUSY);
  }
  uint32_t min = fifo[SVGA_FIFO_MIN], max = fifo[SVGA_FIFO_MAX];
  for (uint32_t i = 0; i < n + ntail; i++) {
    fifo[next / 4] = i < n ? words[i] : tail[i - n];
    next += 4;
    if (next == max)
      next = min;
  }
  fifo[SVGA_FIFO_NEXT_CMD] = next;
  queued = true;
  spin_unlock_irqrestore(&fifo_lock, f);
}

/* Queue SVGA_CMD_UPDATE for a box; the host is kicked once per batch by svga_flush. */
static void svga_present(int x, int y, int w, int h)
{
  if (w <= 0 || h <= 0)
    return;
  uint32_t cmd[5] = {SVGA_CMD_UPDATE, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h};
  fifo_put(cmd, 5, NULL, 0);
}

static void svga_flush(void)
{
  if (!queued)
    return;
  queued = false;
  reg_write(SVGA_REG_SYNC, 1);
}

static int vmsvga_cursor_set(const uint32_t *argb, int w, int h, int hot_x, int hot_y, int scale)
{
  if (!io_base || !fifo)
    return -1;
  if (!argb) {
    if (hw_cursor)
      reg_write(SVGA_REG_CURSOR_ON, 0);
    hw_cursor = false;
    return 0;
  }
  uint32_t caps = reg_read(SVGA_REG_CAPABILITIES);
  if (!(caps & SVGA_CAP_CURSOR_BYPASS) || !(caps & SVGA_CAP_ALPHA_CURSOR))
    return -1;
  if (w <= 0 || h <= 0 || (uint32_t)(w * h + 6) * 4 >= (fifo[SVGA_FIFO_MAX] - fifo[SVGA_FIFO_MIN]) / 2)
    return -1;
  uint32_t head[6] = {SVGA_CMD_DEFINE_ALPHA_CURSOR, 0, (uint32_t)hot_x, (uint32_t)hot_y, (uint32_t)w, (uint32_t)h};
  fifo_put(head, 6, argb, (uint32_t)(w * h));
  svga_flush();
  cursor_scale = scale > 0 ? scale : 1;
  hw_cursor = true;
  return 0;
}

static void vmsvga_cursor_move(int x, int y)
{
  if (!hw_cursor)
    return;
  reg_write(SVGA_REG_CURSOR_X, (uint32_t)(x * cursor_scale));
  reg_write(SVGA_REG_CURSOR_Y, (uint32_t)(y * cursor_scale));
  reg_write(SVGA_REG_CURSOR_ON, 1);
}

int vmsvga_init(uint32_t want_w, uint32_t want_h)
{
  uint32_t bar0 = 0, bar1 = 0, bar2 = 0;
  bool found = false;
  for (int bus = 0; bus < 256 && !found; bus++) {
    for (int slot = 0; slot < 32; slot++) {
      uint32_t id = pci_read32(bus, slot, 0, 0x00);
      if ((id & 0xFFFF) != VMW_VENDOR || ((id >> 16) & 0xFFFF) != VMW_DEV_SVGA2)
        continue;
      bar0 = pci_read32(bus, slot, 0, 0x10);
      bar1 = pci_read32(bus, slot, 0, 0x14);
      bar2 = pci_read32(bus, slot, 0, 0x18);
      uint32_t cmd = pci_read32(bus, slot, 0, 0x04);
      pci_write32(bus, slot, 0, 0x04, cmd | 0x7);
      found = true;
      break;
    }
  }
  if (!found)
    return -1;

  con_write("vmsvga: SVGA II device found (15ad:0405).\n");

  io_base = (uint16_t)(bar0 & 0xFFFFFFFCu); /* I/O BAR */
  uint32_t fb_phys = bar1 & 0xFFFFFFF0u;    /* framebuffer (memory BAR)        */
  uint32_t fifo_phys = bar2 & 0xFFFFFFF0u;  /* command FIFO (memory BAR)       */

  reg_write(SVGA_REG_ID, SVGA_ID_2);
  if (reg_read(SVGA_REG_ID) != SVGA_ID_2) {
    reg_write(SVGA_REG_ID, SVGA_ID_1);
    if (reg_read(SVGA_REG_ID) != SVGA_ID_1) {
      reg_write(SVGA_REG_ID, SVGA_ID_0);
      if (reg_read(SVGA_REG_ID) != SVGA_ID_0) {
        con_write("vmsvga: SVGA ID negotiation failed.\n");
        return -1;
      }
    }
  }

  uint32_t maxw = reg_read(SVGA_REG_MAX_WIDTH);
  uint32_t maxh = reg_read(SVGA_REG_MAX_HEIGHT);
  if (maxw && want_w > maxw)
    want_w = maxw;
  if (maxh && want_h > maxh)
    want_h = maxh;

  reg_write(SVGA_REG_WIDTH, want_w);
  reg_write(SVGA_REG_HEIGHT, want_h);
  reg_write(SVGA_REG_BITS_PER_PIXEL, 32);
  reg_write(SVGA_REG_ENABLE, 1);

  uint32_t pitch = reg_read(SVGA_REG_BYTES_PER_LINE);
  uint32_t fb_off = reg_read(SVGA_REG_FB_OFFSET);
  uint32_t fifo_sz = reg_read(SVGA_REG_MEM_SIZE);
  if (!pitch || !fifo_sz) {
    con_write("vmsvga: bad pitch/FIFO size from device.\n");
    reg_write(SVGA_REG_ENABLE, 0);
    return -1;
  }

  con_printf("vmsvga: mode %ux%ux32, pitch=%u fifo=%uKiB.\n", want_w, want_h, pitch, fifo_sz >> 10);

  /* Map the FIFO ring uncached so pointer writes reach the host. */
  if (paging_identity_map(fifo_phys, fifo_sz) < 0) {
    con_write("vmsvga: FIFO map failed.\n");
    reg_write(SVGA_REG_ENABLE, 0);
    return -1;
  }
  paging_mark_uc(fifo_phys, fifo_sz);
  fifo = (volatile uint32_t *)(uintptr_t)fifo_phys;

  fifo[SVGA_FIFO_MIN] = SVGA_FIFO_NUM_REGS * 4;
  fifo[SVGA_FIFO_MAX] = fifo_sz;
  fifo[SVGA_FIFO_NEXT_CMD] = SVGA_FIFO_NUM_REGS * 4;
  fifo[SVGA_FIFO_STOP] = SVGA_FIFO_NUM_REGS * 4;
  reg_write(SVGA_REG_CONFIG_DONE, 1);

  /* Register the hook before adopting so fb_adopt's initial present is pushed too. */
  fb_set_present_hook(svga_present, svga_flush);
  fb_set_cursor_hooks(vmsvga_cursor_set, vmsvga_cursor_move);

  if (fb_adopt(fb_phys + fb_off, want_w, want_h, pitch, 32) != 0) {
    con_write("vmsvga: fb_adopt failed.\n");
    fb_set_present_hook(NULL, NULL);
    fb_set_cursor_hooks(NULL, NULL);
    reg_write(SVGA_REG_ENABLE, 0);
    return -1;
  }
  return 0;
}
