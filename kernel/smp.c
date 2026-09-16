/* smp.c — read the ACPI MADT ("APIC" table) for CPU count, Local APIC IDs, and
 * the Local/IO APIC bases, via the shared acpi_find_table walk. */

#include "smp.h"
#include "console.h"
#include "gdt.h"
#include "heap.h"
#include "idt.h"
#include "paging.h"
#include "power.h"
#include "sched.h"
#include "timer.h"

/* MADT record types. */
#define MADT_LAPIC 0
#define MADT_IOAPIC 1
#define MADT_LAPIC_OVERRIDE 5

/* Low real-mode page the AP trampoline is copied to; the SIPI vector is its
 * page number. Must match SMP_TRAMP in smp_ap.S and sit in reserved low RAM. */
#define SMP_TRAMP 0x8000
#define AP_STACK 16384

struct cpu_info {
  uint8_t apic_id;
  uint8_t acpi_id;
};

static struct cpu_info cpus[SMP_MAX_CPUS];
static int ncpu;
static uint8_t apic_to_idx[256];         /* Local APIC id -> index into cpus[] */
static int topo_ready;                   /* 1 once apic_to_idx is populated     */
static uint32_t lapic_base = 0xFEE00000; /* architectural default             */
static uint32_t ioapic_base;

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
  __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* Unaligned little-endian reads — MADT records aren't field-aligned. */
static inline uint32_t rd32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

void smp_init(void)
{
  const uint8_t *madt = acpi_find_table("APIC");
  if (!madt) {
    ncpu = 1;
    con_write("smp: ACPI MADT absent; assuming 1 CPU.\n");
    return;
  }

  uint32_t len = rd32(madt + 4); /* sdt_header.length                 */
  lapic_base = rd32(madt + 36);  /* 32-bit local APIC address         */

  /* Records begin after the 36-byte SDT header + 8-byte MADT preamble. */
  for (uint32_t off = 44; off + 2 <= len;) {
    const uint8_t *e = madt + off;
    uint8_t type = e[0];
    uint8_t elen = e[1];
    if (elen < 2 || off + elen > len) /* malformed: don't loop forever     */
      break;

    if (type == MADT_LAPIC) {
      uint32_t flags = rd32(e + 4);
      /* bit0 Enabled, bit1 Online-Capable: usable either way. */
      if ((flags & 3) && ncpu < SMP_MAX_CPUS) {
        cpus[ncpu].acpi_id = e[2];
        cpus[ncpu].apic_id = e[3];
        ncpu++;
      }
    } else if (type == MADT_IOAPIC) {
      if (!ioapic_base)
        ioapic_base = rd32(e + 4);
    } else if (type == MADT_LAPIC_OVERRIDE) {
      lapic_base = (uint32_t)rd64(e + 4);
    }
    off += elen;
  }

  if (ncpu == 0) /* MADT with no usable LAPIC record  */
    ncpu = 1;

  /* Pin the boot CPU to index 0 so smp_this_cpu()'s pre-LAPIC fallback (0) is
   * always the BSP. Initial APIC id comes from CPUID, no LAPIC MMIO needed. */
  uint32_t a, b, c, d;
  cpuid(1, &a, &b, &c, &d);
  uint8_t bsp = (uint8_t)(b >> 24);
  for (int i = 1; i < ncpu; i++)
    if (cpus[i].apic_id == bsp) {
      struct cpu_info t = cpus[0];
      cpus[0] = cpus[i];
      cpus[i] = t;
      break;
    }
  for (int i = 0; i < ncpu; i++)
    apic_to_idx[cpus[i].apic_id] = (uint8_t)i;
  topo_ready = 1;

  con_printf("smp: %u CPU(s); LAPIC @ 0x%08X", (uint32_t)ncpu, lapic_base);
  if (ioapic_base) {
    con_printf(", IOAPIC @ 0x%08X", ioapic_base);
  }
  con_write(".\n");
}

int smp_cpu_count(void) { return ncpu ? ncpu : 1; }

uint8_t smp_apic_id(int i)
{
  if (i < 0 || i >= ncpu)
    return 0;
  return cpus[i].apic_id;
}

uint32_t smp_lapic_base(void) { return lapic_base; }
uint32_t smp_ioapic_base(void) { return ioapic_base; }

/* ----------------------------------------------- application-processor
 * bring-up */

/* Trampoline blob + the slots the BSP stages for each AP before its SIPI. */
extern uint8_t ap_trampoline_start[], ap_trampoline_end[];
extern void lapic_timer_isr(void); /* irq_stubs.S, LAPIC_TIMER_VEC gate */
volatile uint32_t ap_stack_top;
volatile uint32_t ap_arg;
volatile uint32_t ap_cr3; /* kernel page dir; AP enables paging */

static volatile uint32_t ap_started;      /* AP -> BSP: "I'm in ap_main"     */
static volatile uint32_t online_mask = 1; /* bit i set when CPU i is running  */

/* Local APIC registers (offsets into the MMIO page). */
#define LAPIC_ID 0x020
#define LAPIC_EOI 0x0B0
#define LAPIC_SVR 0x0F0
#define LAPIC_ICRLO 0x300
#define LAPIC_ICRHI 0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR 0x390
#define LAPIC_TIMER_DIV 0x3E0
#define LVT_PERIODIC 0x20000 /* LVT timer mode bit 17                     */
#define LVT_MASKED 0x10000   /* LVT mask bit 16                           */

static volatile uint32_t *lapic;
static uint32_t lapic_timer_count; /* periodic count for one tick      */
static volatile uint32_t cpu_ticks[SMP_MAX_CPUS];

static inline uint32_t lapic_rd(uint32_t reg) { return lapic[reg / 4]; }
static inline void lapic_wr(uint32_t reg, uint32_t v) { lapic[reg / 4] = v; }

/* Index of the calling CPU into cpus[]. Uses the CPUID initial-APIC-id, NOT the
 * LAPIC MMIO: this runs from schedule() under an arbitrary task's CR3, and the
 * LAPIC mapping lives only in the kernel directory. Returns 0 until the
 * topology map is built (only the BSP runs before then, pinned to index 0). */
int smp_this_cpu(void)
{
  if (!topo_ready)
    return 0;
  uint32_t a, b, c, d;
  cpuid(1, &a, &b, &c, &d);
  return apic_to_idx[(b >> 24) & 0xFF];
}

static inline uint64_t rdmsr(uint32_t msr)
{
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v)
{
  __asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* Busy-wait µs with IF=1, so the PIT keeps advancing timer_us() and the main
 * task is never descheduled (the BSP must stay running to drive the bring-up).
 */
static void udelay(uint32_t us)
{
  uint32_t start = timer_us();
  while (timer_us() - start < us)
    __asm__ volatile("pause");
}

static void send_ipi(uint8_t apic_id, uint32_t icr_low)
{
  lapic_wr(LAPIC_ICRHI, (uint32_t)apic_id << 24);
  lapic_wr(LAPIC_ICRLO, icr_low);
  for (int i = 0; i < 1000 && (lapic_rd(LAPIC_ICRLO) & (1 << 12)); i++)
    udelay(10); /* spin on the delivery-status bit  */
}

/* Each core soft-enables (and hardware-enables) its own Local APIC. */
static void lapic_enable_local(void)
{
  wrmsr(0x1B, rdmsr(0x1B) | (1 << 11));                    /* IA32_APIC_BASE                   */
  lapic_wr(LAPIC_SVR, lapic_rd(LAPIC_SVR) | 0x100 | 0xFF); /* enable + spurious vec */
}

/* Measure the Local APIC timer rate against the PIT once, on the BSP: count
 * down from max for 10 ms, then derive the periodic reload for one scheduler
 * tick. The bus frequency is shared, so every core uses the same reload. */
static uint32_t lapic_timer_calibrate(void)
{
  lapic_wr(LAPIC_TIMER_DIV, 0x3); /* divide by 16                     */
  lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED);
  lapic_wr(LAPIC_TIMER_INIT, 0xFFFFFFFF);
  udelay(10000);
  uint32_t elapsed = 0xFFFFFFFFu - lapic_rd(LAPIC_TIMER_CUR);
  lapic_wr(LAPIC_TIMER_INIT, 0);           /* stop                             */
  return elapsed / 10 * (1000 / TIMER_HZ); /* per-10ms -> per (1/TIMER_HZ s) */
}

/* Start this core's Local APIC timer ticking periodically into LAPIC_TIMER_VEC.
 */
static void lapic_timer_start(void)
{
  lapic_wr(LAPIC_TIMER_DIV, 0x3);
  lapic_wr(LAPIC_LVT_TIMER, LAPIC_TIMER_VEC | LVT_PERIODIC);
  lapic_wr(LAPIC_TIMER_INIT, lapic_timer_count);
}

/* AP C entry, reached from the trampoline with paging already on (kernel CR3).
 * It adopts the shared IDT and its own TSS, initializes the local scheduler,
 * then starts its Local APIC timer. */
void ap_main(uint32_t idx)
{
  gdt_load_ap();
  idt_load();
  tss_load_ap();
  lapic_enable_local();
  sched_init_cpu((int)idx);
  lapic_timer_start();

  __sync_fetch_and_or(&online_mask, 1u << idx);
  ap_started = 1;
  __asm__ volatile("sti");
  sched_start_cpu((int)idx);
}

/* Per-core timer tick (vector LAPIC_TIMER_VEC), dispatched from irq_handler. */
void smp_lapic_tick(struct registers *r)
{
  (void)r;
  cpu_ticks[smp_this_cpu()]++;
  lapic_wr(LAPIC_EOI, 0);
  schedule();
}

void smp_start_aps(void)
{
  if (ncpu <= 1)
    return;

  paging_identity_map(lapic_base, 0x1000);
  paging_mark_uc(lapic_base, 0x1000);
  lapic = (volatile uint32_t *)lapic_base;

  lapic_enable_local(); /* BSP's own Local APIC             */
  lapic_timer_count = lapic_timer_calibrate();
  idt_set_gate(LAPIC_TIMER_VEC, (uint32_t)lapic_timer_isr, GDT_KERNEL_CODE, 0x8E);

  uint8_t bsp = (uint8_t)(lapic_rd(LAPIC_ID) >> 24);

  ap_cr3 = paging_kernel_phys(); /* APs enable paging with this in the trampoline */
  for (uint8_t *p = ap_trampoline_start; p < ap_trampoline_end; p++)
    ((uint8_t *)SMP_TRAMP)[p - ap_trampoline_start] = *p;
  uint32_t vec = SMP_TRAMP >> 12;

  for (int i = 0; i < ncpu; i++) {
    uint8_t aid = cpus[i].apic_id;
    if (aid == bsp)
      continue;

    void *stack = kmalloc(AP_STACK);
    if (!stack)
      continue;
    ap_stack_top = (uint32_t)stack + AP_STACK;
    ap_arg = (uint32_t)i;
    ap_started = 0;

    send_ipi(aid, 0x4500); /* INIT, level-assert               */
    udelay(10000);         /* 10 ms                            */
    for (int s = 0; s < 2; s++) {
      send_ipi(aid, 0x4600 | vec); /* STARTUP                          */
      udelay(200);
    }

    for (int t = 0; t < 200 && !ap_started; t++)
      udelay(1000); /* up to 200 ms for it to check in  */
  }

  int online = 0;
  for (uint32_t m = online_mask; m; m &= m - 1)
    online++;
  con_printf("smp: %u/%u cores online.\n", (uint32_t)online, (uint32_t)ncpu);
}

int smp_online_count(void)
{
  int n = 0;
  for (uint32_t m = online_mask; m; m &= m - 1)
    n++;
  return n;
}
