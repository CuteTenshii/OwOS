/* arch.c: ARMv7-A platform support: generic timer, exception dispatch and PSCI. */
#include "arch.h"
#include "irqflags.h"

#define CNTV_CTL_ENABLE 1u

static volatile uint32_t ticks;
static uint32_t tick_interval;

int timer_init(void)
{
  uint32_t hz = CP15_READ(CNTFRQ);
  if (hz < TICK_HZ)
    return -1;
  tick_interval = hz / TICK_HZ;
  CP15_WRITE(CNTV_TVAL, tick_interval);
  CP15_WRITE(CNTV_CTL, CNTV_CTL_ENABLE);
  return 0;
}

uint32_t timer_ticks(void) { return ticks; }

static void irq_dispatch(void)
{
  uint32_t iar = gic_ack();
  uint32_t id = iar & 0x3ffu;
  if (id == IRQ_VTIMER_PPI) {
    CP15_WRITE(CNTV_TVAL, tick_interval);
    ticks++;
  }
  if (id < 1020)
    gic_eoi(iar);
  if (id == IRQ_VTIMER_PPI)
    schedule();
}

static const char *const trap_names[] = {"reset",      "undefined instruction", "svc", "prefetch abort",
                                         "data abort", "unused vector",         "irq", "fiq"};

static void exception_fatal(const struct trap_frame *tf, uint32_t kind)
{
  uart_puts("\nexception: ");
  uart_puts(kind < sizeof trap_names / sizeof *trap_names ? trap_names[kind] : "unknown");
  uart_puts(" pc=");
  uart_puthex(tf->pc);
  uart_puts(" cpsr=");
  uart_puthex(tf->cpsr);
  uart_puts(" lr=");
  uart_puthex(tf->lr);
  if (kind == TRAP_DABT) {
    uart_puts(" dfsr=");
    uart_puthex(CP15_READ(DFSR));
    uart_puts(" dfar=");
    uart_puthex(CP15_READ(DFAR));
  } else if (kind == TRAP_PABT) {
    uart_puts(" ifsr=");
    uart_puthex(CP15_READ(IFSR));
    uart_puts(" ifar=");
    uart_puthex(CP15_READ(IFAR));
  }
  for (int i = 0; i < 13; i++) {
    uart_puts(i % 4 ? " r" : "\n r");
    uart_putc(i < 10 ? (char)('0' + i) : '1');
    if (i >= 10)
      uart_putc((char)('0' + i - 10));
    uart_putc('=');
    uart_puthex(tf->r[i]);
  }
  uart_putc('\n');
  for (;;)
    cpu_halt();
}

volatile uint32_t svc_count;
struct fault_probe fault_probe;

static int fault_recover(struct trap_frame *tf, uint32_t kind)
{
  if (!fault_probe.armed)
    return 0;
  fault_probe.kind = kind;
  fault_probe.status = kind == TRAP_DABT ? CP15_READ(DFSR) : kind == TRAP_PABT ? CP15_READ(IFSR) : 0;
  fault_probe.addr = kind == TRAP_DABT ? CP15_READ(DFAR) : kind == TRAP_PABT ? CP15_READ(IFAR) : tf->pc;
  fault_probe.armed = 0;
  tf->pc += 4; /* the kernel is built for ARM state: every instruction is 4 bytes */
  return 1;
}

void arch_exception(struct trap_frame *tf, uint32_t kind)
{
  switch (kind) {
  case TRAP_IRQ:
    irq_dispatch();
    return;
  case TRAP_SVC:
    svc_count++;
    return;
  case TRAP_UNDEF:
  case TRAP_PABT:
  case TRAP_DABT:
    if (fault_recover(tf, kind))
      return;
    break;
  default:
    break;
  }
  exception_fatal(tf, kind);
}

#define PSCI_SYSTEM_OFF 0x84000008u

/* SMC Calling Convention: r0-r3 carry the call and its results, everything else is preserved. */
static void psci_call(uint32_t fn)
{
  register uint32_t r0 __asm__("r0") = fn;
  if (plat.psci_smc < 0 ? boot_mode == PSR_MODE_HYP : plat.psci_smc)
    __asm__ volatile(".arch_extension sec\n\tsmc #0" : "+r"(r0) : : "r1", "r2", "r3", "memory");
  else
    __asm__ volatile(".arch_extension virt\n\thvc #0" : "+r"(r0) : : "r1", "r2", "r3", "memory");
}

void arch_poweroff(void)
{
  psci_call(PSCI_SYSTEM_OFF);
  for (;;)
    cpu_halt();
}

uintptr_t context_init(void *stack_top, void (*entry)(void *), void *arg)
{
  uint32_t *sp = (uint32_t *)((uintptr_t)stack_top - 40);
  for (int i = 0; i < 10; i++)
    sp[i] = 0;
  sp[0] = (uint32_t)(uintptr_t)entry;             /* r4, called by the trampoline */
  sp[1] = (uint32_t)(uintptr_t)arg;               /* r5 */
  sp[9] = (uint32_t)(uintptr_t)thread_trampoline; /* lr */
  return (uintptr_t)sp;
}
