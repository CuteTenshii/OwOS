/* selftest.c: boots-in-QEMU checks for the 32-bit ARM kernel: what only a running system can show (MMU and caches,
 * exceptions, interrupt masking, timer interrupts, scheduler). Pure logic is unit-tested on the host: see
 * tests/fdt_test.c, tests/memmap_test.c and tests/pmm_test.c. Run with `make ARCH=arm check`. */
#include "arch.h"
#include "heap.h"
#include "kstring.h"
#include "spinlock.h"

#define FSR_ALIGNMENT 0x01u

static int failures;

static void check(const char *name, int ok)
{
  uart_puts(ok ? "[ ok ] " : "[FAIL] ");
  uart_puts(name);
  uart_putc('\n');
  if (!ok)
    failures++;
}

static uint32_t fsr_status(uint32_t fsr) { return (fsr & 0xfu) | ((fsr >> 6) & 0x10u); }

/* An SVC taken from SVC mode overwrites lr with its own return address. */
static void svc_call(void) { __asm__ volatile("svc #0" ::: "lr", "memory"); }

static void test_exceptions(void)
{
  uint32_t sctlr = CP15_READ(SCTLR);
  check("MMU and caches enabled", (sctlr & (SCTLR_M | SCTLR_C | SCTLR_I)) == (SCTLR_M | SCTLR_C | SCTLR_I));

  svc_call();
  svc_call();
  check("svc round trip", svc_count == 2);

  fault_probe.armed = 1;
  __asm__ volatile("udf #0" ::: "memory");
  check("undefined instruction is trapped and skipped", !fault_probe.armed && fault_probe.kind == TRAP_UNDEF);

  static uint32_t words[4];
  uintptr_t odd = (uintptr_t)&words[1] + 1;
  fault_probe.armed = 1;
  __asm__ volatile("ldm %0, {r2, r3}" : : "r"(odd) : "r2", "r3", "memory");
  check("unaligned LDM raises a data abort with the faulting address",
        !fault_probe.armed && fault_probe.kind == TRAP_DABT && fsr_status(fault_probe.status) == FSR_ALIGNMENT &&
            fault_probe.addr == odd);
}

static void test_interrupts(void)
{
  uint32_t start = timer_ticks();
  uint64_t c0 = counter_now();
  while (timer_ticks() - start < 10)
    __asm__ volatile("wfi");
  uint64_t elapsed = counter_now() - c0;
  check("timer interrupts via the GIC", timer_ticks() - start >= 10);
  uint64_t expect = CP15_READ(CNTFRQ) / 10; /* 10 ticks at 100 Hz */
  check("tick rate is 100 Hz on the generic counter", elapsed >= expect / 2 && elapsed <= expect * 2);

  /* Two tick periods with interrupts masked: none may be taken, and the pending one arrives on restore. */
  uintptr_t flags = irq_save();
  uint32_t masked_start = timer_ticks();
  uint64_t until = counter_now() + CP15_READ(CNTFRQ) / 50;
  while (counter_now() < until)
    ;
  int stayed = timer_ticks() == masked_start;
  irq_restore(flags);
  for (int i = 0; i < 1000 && timer_ticks() == masked_start; i++)
    __asm__ volatile("wfi");
  check("irq_save masks the tick and irq_restore lets it in", stayed && timer_ticks() != masked_start);
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
  check("RAM size read from the DTB", pmm_installed_kib() == 256 * 1024);

  uint32_t before = free_frames();
  phys_t f = pmm_alloc_frame();
  check("pmm frame is usable memory", f && !(f & (PMM_FRAME_SIZE - 1)) && free_frames() == before - 1);
  memset((void *)f, 0xa5, PMM_FRAME_SIZE);
  check("frame holds data under the MMU and caches",
        ((uint8_t *)f)[0] == 0xa5 && ((uint8_t *)f)[PMM_FRAME_SIZE - 1] == 0xa5);
  pmm_free_frame(f);

  size_t heap_before = heap_free_bytes();
  uint8_t *p = kmalloc(100), *q = kmalloc(200);
  memset(p, 1, 100);
  memset(q, 2, 200);
  check("kmalloc blocks hold data", p && q && p[99] == 1 && q[0] == 2 && q[199] == 2);
  kfree(p);
  kfree(q);
  check("heap returns to its size after kfree", free_frames() == before && heap_free_bytes() == heap_before);
}

#define SPINNERS 4
#define WORKERS 4
#define LOCK_ROUNDS 500
#define ALLOC_ROUNDS 300
#define MANY_THREADS 400

static volatile int stop;
static spinlock_t shared_lock;
static volatile uint32_t shared_counter;
static volatile int alloc_errors;
static volatile uint32_t many_done;

static void spinner(void *arg)
{
  volatile uint32_t *counter = arg;
  while (!stop)
    (*counter)++;
}

/* The delay inside the critical section makes a tick land there often. */
static void locked_incrementer(void *arg)
{
  (void)arg;
  for (int i = 0; i < LOCK_ROUNDS; i++) {
    uintptr_t f = spin_lock_irqsave(&shared_lock);
    uint32_t v = shared_counter;
    for (int k = 0; k < 3000; k++)
      __asm__ volatile("" ::: "memory");
    shared_counter = v + 1;
    spin_unlock_irqrestore(&shared_lock, f);
  }
}

static void allocator(void *arg)
{
  uint8_t me = (uint8_t)(uintptr_t)arg;
  for (int i = 0; i < ALLOC_ROUNDS; i++) {
    uint8_t *p = kmalloc(64 + (size_t)i % 200);
    if (!p) {
      alloc_errors++;
      continue;
    }
    memset(p, me, 64);
    thread_yield();
    for (int j = 0; j < 64; j++)
      if (p[j] != me)
        alloc_errors++;
    phys_t f = pmm_alloc_frame();
    if (!f)
      alloc_errors++;
    pmm_free_frame(f);
    kfree(p);
  }
}

static void exits_seven(void *arg)
{
  (void)arg;
  thread_exit(7);
}

static void quick(void *arg)
{
  (void)arg;
  many_done++;
}

static void run_all(void (*fn)(void *), int n)
{
  int *ids = kmalloc((size_t)n * sizeof *ids);
  for (int i = 0; i < n; i++)
    ids[i] = thread_create("worker", fn, (void *)(uintptr_t)i);
  for (int i = 0; i < n; i++)
    thread_join(ids[i]);
  kfree(ids);
}

static void test_sched(void)
{
  uint32_t frames = pmm_free_kib();
  size_t heap = heap_free_bytes();

  int ids[SPINNERS];
  uint32_t counters[SPINNERS] = {0};
  for (int i = 0; i < SPINNERS; i++)
    ids[i] = thread_create("spin", spinner, &counters[i]);
  thread_sleep(30);
  stop = 1;
  int all_ran = 1;
  for (int i = 0; i < SPINNERS; i++) {
    thread_join(ids[i]);
    all_ran &= counters[i] != 0;
  }
  check("busy threads are preempted and all make progress", all_ran);

  check("thread_exit code reaches join", thread_join(thread_create("exit7", exits_seven, 0)) == 7);
  check("join of an unknown thread fails", thread_join(9999) == -1);

  run_all(quick, MANY_THREADS);
  check("hundreds of threads at once", many_done == MANY_THREADS);

  run_all(locked_incrementer, WORKERS);
  check("spinlock keeps preempted updates whole", shared_counter == (uint32_t)WORKERS * LOCK_ROUNDS);

  run_all(allocator, WORKERS);
  check("interleaved pmm and heap use stays consistent", alloc_errors == 0);

  check("scheduler releases frames and stacks", pmm_free_kib() == frames && heap_free_bytes() == heap);
}

int selftest_run(void)
{
  test_exceptions();
  test_interrupts();
  test_kstring();
  test_memory();
  test_sched();

  uart_puts(failures ? "ARM SELFTEST FAIL\n" : "ARM SELFTEST PASS\n");
  return failures;
}
