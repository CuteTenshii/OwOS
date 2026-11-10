/* smp.c: bring up secondary CPUs through PSCI. */
#include "arch.h"

static volatile int online = 1;

int smp_online(void) { return online; }

void secondary_main(int cpu)
{
  gic_cpu_init();
  timer_init();
  sched_cpu_init(cpu);
  __sync_fetch_and_add(&online, 1);
  __asm__ volatile("msr daifclr, #2");
  for (;;) {
    thread_yield();
    __asm__ volatile("wfi");
  }
}

int smp_start(void)
{
  int started = 1;
  for (int cpu = 1; cpu < MAX_CPUS; cpu++)
    if (psci_cpu_on(cpu) == 0)
      started++;

  for (uint64_t spins = 0; online < started && spins < 100000000; spins++)
    __asm__ volatile("yield");
  return online;
}
