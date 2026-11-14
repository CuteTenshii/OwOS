/* smp.c: bring up secondary CPUs through PSCI into the shared scheduler. */
#include "smp.h"
#include "arch.h"
#include "fpu.h"
#include "irqflags.h"
#include "sched.h"
#include "timer.h"

static volatile int online = 1;

int smp_this_cpu(void) { return cpu_id(); }

uint32_t smp_cpu_count(void) { return (uint32_t)online; }

/* A secondary CPU's C entry (boot.S), on its boot stack, which becomes its idle task's. */
void secondary_main(int cpu)
{
  gic_cpu_init();
  fpu_cpu_init();
  timer_init();
  sched_init_cpu(cpu);
  __sync_fetch_and_add(&online, 1);
  irq_enable();
  sched_start_cpu(cpu);
}

int smp_start(void)
{
  int started = 1;
  for (int cpu = 1; cpu < plat.ncpus; cpu++)
    if (psci_cpu_on(cpu) == 0)
      started++;
  for (uint64_t spins = 0; online < started && spins < 100000000; spins++)
    cpu_relax();
  return online;
}
