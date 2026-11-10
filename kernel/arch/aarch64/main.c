/* main.c: AArch64 bring-up and boot self-test. */
#include "arch.h"
#include "kstring.h"

extern volatile uint64_t svc_count;

static uintptr_t main_sp, worker_sp;
static uint8_t worker_stack[4096] __attribute__((aligned(16)));
static volatile int worker_runs;

static void worker(void)
{
  for (;;) {
    worker_runs++;
    context_switch(&worker_sp, main_sp);
  }
}

static int failures;

static void check(const char *name, int ok)
{
  uart_puts(ok ? "[ ok ] " : "[FAIL] ");
  uart_puts(name);
  uart_putc('\n');
  if (!ok)
    failures++;
}

static void test_kstring(void)
{
  char buf[32];
  char src[] = "abcdefgh";
  memcpy(buf, src, sizeof src);
  check("memcpy", memcmp(buf, src, sizeof src) == 0);
  memmove(buf + 2, buf, 6);
  check("memmove overlap", memcmp(buf, "ababcdef", 8) == 0);
  memset(buf, 'z', 5);
  check("memset", buf[0] == 'z' && buf[4] == 'z' && buf[5] == 'd');
  ksnprintf(buf, sizeof buf, "%s-%d-%x", "os", 42, 255);
  check("ksnprintf", streq(buf, "os-42-ff"));
}

void arch_main(void)
{
  uart_init();
  uart_puts("OwOS aarch64 boot, EL");
  uart_putc('0' + (char)boot_el);
  uart_puts(" -> EL1, MIDR ");
  uart_puthex(SYSREG_READ(midr_el1));
  uart_putc('\n');

  mmu_init();
  check("mmu enabled", SYSREG_READ(sctlr_el1) & 1);

  __asm__ volatile("svc #0");
  __asm__ volatile("svc #0");
  check("synchronous exception round trip", svc_count == 2);

  test_kstring();

  gic_init();
  timer_init();
  __asm__ volatile("msr daifclr, #2");
  while (timer_ticks() < 10)
    __asm__ volatile("wfi");
  check("timer interrupts via GIC", timer_ticks() >= 10);

  worker_sp = context_init(worker_stack + sizeof worker_stack, worker);
  for (int i = 0; i < 5; i++)
    context_switch(&main_sp, worker_sp);
  check("context switch", worker_runs == 5);

  uart_puts(failures ? "AARCH64 SELFTEST FAIL\n" : "AARCH64 SELFTEST PASS\n");
  arch_poweroff();
}
