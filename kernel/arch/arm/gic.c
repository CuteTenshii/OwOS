/* gic.c: GICv2 distributor and memory-mapped CPU interface. */
#include "arch.h"

#define IRQ_PRIORITY 0x80u

#define GICD_CTLR 0x000
#define GICD_TYPER 0x004
#define GICD_ISENABLER 0x100
#define GICD_ICENABLER 0x180
#define GICD_IPRIORITYR 0x400

#define GICC_CTLR 0x000
#define GICC_PMR 0x004
#define GICC_IAR 0x00c
#define GICC_EOIR 0x010

void gic_init(void)
{
  uintptr_t d = plat.gicd_base, c = plat.gicc_base;
  mmio_write32(d + GICD_CTLR, 0);

  /* Nothing the loader left enabled may fire without a handler. */
  uint32_t words = (mmio_read32(d + GICD_TYPER) & 0x1fu) + 1;
  for (uint32_t i = 0; i < words; i++)
    mmio_write32(d + GICD_ICENABLER + 4 * i, ~0u);

  uintptr_t prio = d + GICD_IPRIORITYR + (IRQ_VTIMER_PPI & ~3u);
  uint32_t shift = 8 * (IRQ_VTIMER_PPI & 3u);
  mmio_write32(prio, (mmio_read32(prio) & ~(0xffu << shift)) | IRQ_PRIORITY << shift);
  mmio_write32(d + GICD_ISENABLER, 1u << IRQ_VTIMER_PPI);

  mmio_write32(d + GICD_CTLR, 1);
  mmio_write32(c + GICC_PMR, 0xff);
  mmio_write32(c + GICC_CTLR, 1);
}

uint32_t gic_ack(void) { return mmio_read32(plat.gicc_base + GICC_IAR); }

void gic_eoi(uint32_t iar) { mmio_write32(plat.gicc_base + GICC_EOIR, iar); }
