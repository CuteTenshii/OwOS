/* vmsvga.c — VMware SVGA II display adapter. See vmsvga.h.
 *
 * Three PCI BARs: BAR0 is an I/O register window (index/value port pair), BAR1
 * the linear framebuffer, BAR2 the command FIFO (a ring in device memory). We
 * negotiate the SVGA version, program a 32-bpp mode, then let fb.c own the
 * framebuffer exactly as if a bootloader had set it (fb_adopt). The one piece
 * SVGA needs beyond a dumb framebuffer is an explicit "this rectangle changed"
 * message per frame: fb_present calls our hook with the damage box and we push
 * an SVGA_CMD_UPDATE through the FIFO so the host re-reads those pixels. */

#include "vmsvga.h"
#include "console.h"
#include "fb.h"
#include "io.h"
#include "paging.h"
#include "pci.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VMW_VENDOR 0x15AD
#define VMW_DEV_SVGA2 0x0405

/* BAR0 I/O port offsets. */
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
};

/* FIFO control words live in the first four dwords of the FIFO region. */
enum {
  SVGA_FIFO_MIN = 0,
  SVGA_FIFO_MAX = 1,
  SVGA_FIFO_NEXT_CMD = 2,
  SVGA_FIFO_STOP = 3,
};

#define SVGA_FIFO_NUM_REGS 4
#define SVGA_CMD_UPDATE 1

/* Version negotiation magic. Offer ID_2 first and step down until one reads
 * back, so we match whatever the device supports. */
#define SVGA_MAGIC 0x900000u
#define SVGA_ID_2 ((SVGA_MAGIC << 8) | 2)
#define SVGA_ID_1 ((SVGA_MAGIC << 8) | 1)
#define SVGA_ID_0 ((SVGA_MAGIC << 8) | 0)

static uint16_t io_base;
static volatile uint32_t *fifo;
static uint32_t fifo_words; /* FIFO size in dwords                            */

static void reg_write(uint32_t reg, uint32_t val)
{
  outl(io_base + SVGA_INDEX_PORT, reg);
  outl(io_base + SVGA_VALUE_PORT, val);
}

static uint32_t reg_read(uint32_t reg)
{
  outl(io_base + SVGA_INDEX_PORT, reg);
  return inl(io_base + SVGA_VALUE_PORT);
}

/* Append one dword to the FIFO ring, wrapping at MAX and stalling (via a forced
 * host sync) if it would overrun the host's read pointer. Our commands are tiny
 * and we kick the host after each, so the stall path is essentially never hit.
 */
static void fifo_write(uint32_t value)
{
  uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
  uint32_t max = fifo[SVGA_FIFO_MAX];
  uint32_t min = fifo[SVGA_FIFO_MIN];

  uint32_t after = next + 4;
  if (after == max)
    after = min;
  while (after == fifo[SVGA_FIFO_STOP]) { /* full: let the host catch up    */
    reg_write(SVGA_REG_SYNC, 1);
    (void)reg_read(SVGA_REG_BUSY);
  }

  fifo[next / 4] = value;
  fifo[SVGA_FIFO_NEXT_CMD] = after;
}

/* fb.c present hook: tell the host which rectangle of the framebuffer changed.
 */
static void svga_present(int x, int y, int w, int h)
{
  if (w <= 0 || h <= 0)
    return;
  fifo_write(SVGA_CMD_UPDATE);
  fifo_write((uint32_t)x);
  fifo_write((uint32_t)y);
  fifo_write((uint32_t)w);
  fifo_write((uint32_t)h);
  reg_write(SVGA_REG_SYNC, 1); /* nudge the host to drain the FIFO */
}

int vmsvga_init(uint32_t want_w, uint32_t want_h)
{
  /* Find the device and capture its three BARs. */
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
      /* Enable I/O + memory decode + bus mastering. */
      uint32_t cmd = pci_read32(bus, slot, 0, 0x04);
      pci_write32(bus, slot, 0, 0x04, cmd | 0x7);
      found = true;
      break;
    }
  }
  if (!found)
    return -1; /* not a VMware guest: stay quiet  */

  con_write("vmsvga: SVGA II device found (15ad:0405).\n");

  io_base = (uint16_t)(bar0 & 0xFFFFFFFCu); /* I/O BAR */
  uint32_t fb_phys = bar1 & 0xFFFFFFF0u;    /* framebuffer (memory BAR)        */
  uint32_t fifo_phys = bar2 & 0xFFFFFFF0u;  /* command FIFO (memory BAR)       */

  /* Negotiate the highest SVGA version the device echoes back. */
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

  /* Clamp the requested mode to what the adapter allows. */
  uint32_t maxw = reg_read(SVGA_REG_MAX_WIDTH);
  uint32_t maxh = reg_read(SVGA_REG_MAX_HEIGHT);
  if (maxw && want_w > maxw)
    want_w = maxw;
  if (maxh && want_h > maxh)
    want_h = maxh;

  /* Program and enable the mode, then read back the resulting pitch/offset. */
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

  /* Map and initialise the FIFO ring. Device memory: map it uncached so our
   * pointer writes reach the host without lingering in a cache line. */
  if (paging_identity_map(fifo_phys, fifo_sz) < 0) {
    con_write("vmsvga: FIFO map failed.\n");
    reg_write(SVGA_REG_ENABLE, 0);
    return -1;
  }
  paging_mark_uc(fifo_phys, fifo_sz);
  fifo = (volatile uint32_t *)(uintptr_t)fifo_phys;
  fifo_words = fifo_sz / 4;

  fifo[SVGA_FIFO_MIN] = SVGA_FIFO_NUM_REGS * 4;
  fifo[SVGA_FIFO_MAX] = fifo_sz;
  fifo[SVGA_FIFO_NEXT_CMD] = SVGA_FIFO_NUM_REGS * 4;
  fifo[SVGA_FIFO_STOP] = SVGA_FIFO_NUM_REGS * 4;
  reg_write(SVGA_REG_CONFIG_DONE, 1);

  /* Register the damage-flush hook before adopting, so fb_adopt's own initial
   * clear-and-present is pushed to the host too. */
  fb_set_present_hook(svga_present);

  if (fb_adopt(fb_phys + fb_off, want_w, want_h, pitch, 32) != 0) {
    con_write("vmsvga: fb_adopt failed.\n");
    fb_set_present_hook(NULL);
    reg_write(SVGA_REG_ENABLE, 0);
    return -1;
  }
  return 0;
}
