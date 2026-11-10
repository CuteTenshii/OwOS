/* main.c: AArch64 bring-up and boot self-test. */
#include "arch.h"
#include "heap.h"
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

static uint32_t free_frames(void) { return pmm_free_kib() / (PMM_FRAME_SIZE / 1024); }

static void test_memory(void)
{
  check("memory init from DTB", arch_mem_init((const void *)boot_dtb) == 0);
  check("pmm sized to 256 MiB RAM", pmm_installed_kib() == 256 * 1024);

  uint32_t before = free_frames();
  phys_t a = pmm_alloc_frame(), b = pmm_alloc_frame();
  check("pmm distinct aligned frames", a && b && a != b && !(a & (PMM_FRAME_SIZE - 1)) && free_frames() == before - 2);
  memset((void *)a, 0xa5, PMM_FRAME_SIZE);
  pmm_free_frame(a);
  check("pmm reuses freed frame", pmm_alloc_frame() == a && free_frames() == before - 2);

  pmm_retain_frame(b);
  check("pmm refcount", pmm_frame_refs(b) == 2);
  pmm_free_frame(b);
  check("pmm shared frame survives one free", pmm_frame_refs(b) == 1 && free_frames() == before - 2);
  pmm_free_frame(b);
  pmm_free_frame(a);
  check("pmm frees return to pool", free_frames() == before);

  phys_t run = pmm_alloc_contig(5 * PMM_FRAME_SIZE);
  check("pmm contiguous run", run && free_frames() == before - 5);
  pmm_free_contig(run, 5 * PMM_FRAME_SIZE);
  check("pmm contiguous free", free_frames() == before);

  size_t heap_before = heap_free_bytes();
  uint8_t *p = kmalloc(100), *q = kmalloc(200);
  check("kmalloc aligned and distinct", p && q && p != q && !((uintptr_t)p & 7) && !((uintptr_t)q & 7));
  memset(p, 1, 100);
  memset(q, 2, 200);
  check("kmalloc blocks do not overlap", p[99] == 1 && q[0] == 2);
  kfree(p);
  uint8_t *r = kmalloc(50);
  memset(r, 3, 50);
  check("kmalloc after kfree leaves live blocks intact", r && q[0] == 2 && q[199] == 2);
  kfree(r);
  kfree(q);
  kfree(q);
  check("kfree coalesces and ignores double free", heap_free_bytes() == heap_before);
}

extern const char user_hello_start[], user_hello_end[], user_fault_start[], user_fault_end[];

static void test_user(void)
{
  uint32_t free_before = free_frames();
  int code = task_run(user_hello_start, (uint32_t)(user_hello_end - user_hello_start));
  check("EL0 task: write syscall and exit code", code == 42);
  code = task_run(user_fault_start, (uint32_t)(user_fault_end - user_fault_start));
  check("EL0 task: kernel access faults and is killed", code == -1);
  check("task frames released", free_frames() == free_before);
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
  test_memory();

  test_user();

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
