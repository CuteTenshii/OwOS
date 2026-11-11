/* gic.c: GICv2 (memory-mapped CPU interface) and GICv3 (redistributors, system register CPU interface). */
#include "arch.h"

#define IRQ_PRIORITY 0x80u

#define GICD_CTLR 0x000
#define GICD_TYPER 0x004
#define GICD_ISENABLER0 0x100
#define GICD_ICENABLER 0x180
#define GICD_IPRIORITYR 0x400
#define GICD_CTLR_RWP (1u << 31)

#define GICC_CTLR 0x000
#define GICC_PMR 0x004
#define GICC_IAR 0x00c
#define GICC_EOIR 0x010

#define GICR_CTLR 0x00
#define GICR_TYPER 0x08
#define GICR_WAKER 0x14
#define GICR_SGI 0x10000
#define GICR_IGROUPR0 0x80
#define GICR_ISENABLER0 0x100
#define GICR_ICENABLER0 0x180
#define GICR_IPRIORITYR 0x400
#define GICR_CTLR_RWP (1u << 3)
#define GICR_WAKER_SLEEP (1u << 1)
#define GICR_WAKER_ASLEEP (1u << 2)
#define GICR_STRIDE 0x20000
#define GICR_TYPER_VLPIS (1u << 1)
#define GICR_TYPER_LAST (1u << 4)

static uintptr_t rdist[MAX_CPUS];

static void dist_wait_rwp(void)
{
  while (mmio_read32(plat.gicd_base + GICD_CTLR) & GICD_CTLR_RWP)
    ;
}

/* Disable every SPI so nothing the loader left enabled can fire without a handler. */
static void dist_quiesce(void)
{
  uint32_t lines = ((mmio_read32(plat.gicd_base + GICD_TYPER) & 0x1f) + 1) * 32;
  for (uint32_t i = 1; i < lines / 32; i++)
    mmio_write32(plat.gicd_base + GICD_ICENABLER + 4 * i, ~0u);
}

/* The redistributor frame whose affinity matches this CPU, or 0. */
static uintptr_t rdist_find(void)
{
  uint64_t self = SYSREG_READ(mpidr_el1) & FDT_MPIDR_AFFINITY;
  uint32_t aff = (uint32_t)(((self >> 32) & 0xff) << 24 | (self & 0xffffff));
  uintptr_t p = plat.gic_second_base, end = p + plat.gic_second_size;
  while (p + GICR_STRIDE <= end) {
    uint64_t typer = mmio_read64(p + GICR_TYPER);
    if ((uint32_t)(typer >> 32) == aff)
      return p;
    if (typer & GICR_TYPER_LAST)
      break;
    p += GICR_STRIDE * ((typer & GICR_TYPER_VLPIS) ? 2 : 1);
  }
  return 0;
}

static void v3_cpu_init(void)
{
  uintptr_t rd = rdist_find();
  if (!rd) {
    uart_puts("gic: no redistributor for this CPU\n");
    return;
  }
  rdist[cpu_id()] = rd;

  mmio_write32(rd + GICR_WAKER, mmio_read32(rd + GICR_WAKER) & ~GICR_WAKER_SLEEP);
  while (mmio_read32(rd + GICR_WAKER) & GICR_WAKER_ASLEEP)
    ;

  uintptr_t sgi = rd + GICR_SGI;
  mmio_write32(sgi + GICR_ICENABLER0, ~0u);
  while (mmio_read32(rd + GICR_CTLR) & GICR_CTLR_RWP)
    ;
  mmio_write32(sgi + GICR_IGROUPR0, ~0u);
  for (int i = 0; i < 32; i += 4)
    mmio_write32(sgi + GICR_IPRIORITYR + (uint32_t)i, IRQ_PRIORITY * 0x01010101u);
  mmio_write32(sgi + GICR_ISENABLER0, 1u << IRQ_TIMER_PPI);

  SYSREG_WRITE(icc_sre_el1, SYSREG_READ(icc_sre_el1) | 1);
  __asm__ volatile("isb");
  SYSREG_WRITE(icc_pmr_el1, 0xff);
  SYSREG_WRITE(icc_igrpen1_el1, 1);
  __asm__ volatile("isb");
}

static void v2_cpu_init(void)
{
  mmio_write32(plat.gicd_base + GICD_IPRIORITYR + (IRQ_TIMER_PPI & ~3), IRQ_PRIORITY << (8 * (IRQ_TIMER_PPI & 3)));
  mmio_write32(plat.gicd_base + GICD_ISENABLER0, 1u << IRQ_TIMER_PPI);
  mmio_write32(plat.gic_second_base + GICC_PMR, 0xff);
  mmio_write32(plat.gic_second_base + GICC_CTLR, 1);
}

/* Per-CPU part: the PPI enable/priority registers and the CPU interface are banked. */
void gic_cpu_init(void)
{
  if (plat.gic_kind == GIC_V3)
    v3_cpu_init();
  else
    v2_cpu_init();
}

void gic_init(void)
{
  mmio_write32(plat.gicd_base + GICD_CTLR, 0);
  if (plat.gic_kind == GIC_V3) {
    dist_wait_rwp();
    dist_quiesce();
    mmio_write32(plat.gicd_base + GICD_CTLR, (1u << 4) | (1u << 1) | 1u); /* ARE, Group 1 and Group 0 */
    dist_wait_rwp();
  } else {
    mmio_write32(plat.gicd_base + GICD_CTLR, 1);
  }
  gic_cpu_init();
}

uint32_t gic_ack(void)
{
  if (plat.gic_kind == GIC_V3)
    return (uint32_t)SYSREG_READ(icc_iar1_el1) & 0xffffff;
  return mmio_read32(plat.gic_second_base + GICC_IAR);
}

void gic_eoi(uint32_t iar)
{
  if (plat.gic_kind == GIC_V3)
    SYSREG_WRITE(icc_eoir1_el1, iar);
  else
    mmio_write32(plat.gic_second_base + GICC_EOIR, iar);
}
