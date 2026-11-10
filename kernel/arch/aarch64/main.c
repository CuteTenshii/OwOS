/* main.c: AArch64 bring-up and boot self-test. */
#include "arch.h"
#include "heap.h"
#include "kstring.h"
#include "spinlock.h"

extern volatile uint64_t svc_count;

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

extern const char user_hello_start[], user_hello_end[], user_fault_start[], user_fault_end[], user_sys_start[],
    user_sys_end[];

static void test_user(void)
{
  uint32_t free_before = free_frames();
  int code = task_run(user_hello_start, (uint32_t)(user_hello_end - user_hello_start));
  check("EL0 task: write syscall and exit code", code == 42);
  code = task_run(user_fault_start, (uint32_t)(user_fault_end - user_fault_start));
  check("EL0 task: kernel access faults and is killed", code == -1);
  code = task_run(user_sys_start, (uint32_t)(user_sys_end - user_sys_start));
  check("syscalls: errors, unknown numbers, msec/sleep, getpid", code == 0);
  check("task frames released", free_frames() == free_before);
}

extern const char hello_elf_start[], hello_elf_end[], segv_elf_start[], segv_elf_end[], nx_elf_start[], nx_elf_end[];

#define ELF(n) n##_elf_start, (uint64_t)(n##_elf_end - n##_elf_start)

static void patch(uint8_t *buf, uint64_t off, uint64_t val, int width) { memcpy(buf + off, &val, (uint32_t)width); }

/* Loads a copy of hello.elf with one field overwritten; true if the loader refused it and freed everything. */
static bool rejected(uint64_t off, uint64_t val, int width, uint64_t size)
{
  uint64_t full = (uint64_t)(hello_elf_end - hello_elf_start);
  uint8_t *buf = kmalloc((size_t)full);
  struct user_image img;
  uint32_t before = free_frames();
  memcpy(buf, hello_elf_start, (uint32_t)full);
  if (width)
    patch(buf, off, val, width);
  bool ok = elf_load(&img, buf, size ? size : full) != 0 && free_frames() == before;
  kfree(buf);
  return ok;
}

static void test_elf(void)
{
  uint32_t frames = free_frames();
  int code = -99;
  check("ELF: hello loads, runs and its self-checks pass", task_run_elf(ELF(hello), &code) == 0 && code == 0);
  code = -99;
  check("ELF: write to .rodata kills the task", task_run_elf(ELF(segv), &code) == 0 && code == -1);
  code = -99;
  check("ELF: executing .data kills the task", task_run_elf(ELF(nx), &code) == 0 && code == -1);
  check("ELF: task frames released", free_frames() == frames);

  uint64_t ph0;
  memcpy(&ph0, hello_elf_start + 32, sizeof ph0); /* e_phoff */
  check("ELF: bad magic rejected", rejected(0, 0, 1, 0));
  check("ELF: wrong machine rejected", rejected(18, 62, 2, 0));
  check("ELF: truncated file rejected", rejected(0, 0, 0, 100));
  check("ELF: program headers past EOF rejected", rejected(32, 1ULL << 40, 8, 0));
  check("ELF: writable+executable segment rejected", rejected(ph0 + 4, 7, 4, 0));
  check("ELF: segment below the user window rejected", rejected(ph0 + 16, 0x1000, 8, 0));
  check("ELF: filesz > memsz rejected", rejected(ph0 + 32, 1ULL << 20, 8, 0));
  check("ELF: segment reaching the stack rejected", rejected(ph0 + 40, 1ULL << 28, 8, 0));
  check("ELF: entry outside executable segments rejected", rejected(24, USER_BASE + 0x80000, 8, 0));
  check("ELF: rejects leave no frames behind", free_frames() == frames);
}

#define LOCK_ROUNDS 500
#define ALLOC_ROUNDS 300
#define MANY_THREADS 400

static volatile int stop;
static volatile uint32_t cpus_seen;
static spinlock_t shared_lock;
static volatile uint32_t shared_counter;
static volatile int alloc_errors;
static volatile uint32_t many_done;

static void spinner(void *arg)
{
  volatile uint32_t *counter = arg;
  while (!stop) {
    (*counter)++;
    __sync_fetch_and_or(&cpus_seen, 1u << cpu_id());
  }
}

/* The delay inside the critical section makes a missing lock lose updates. */
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
  __sync_fetch_and_add(&many_done, 1);
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
  int cpus = smp_online();
  check("secondary CPUs came online", cpus > 1);

  int n = 2 * cpus;
  int *ids = kmalloc((size_t)n * sizeof *ids);
  uint32_t *counters = kmalloc((size_t)n * sizeof *counters);
  memset(counters, 0, (size_t)n * sizeof *counters);
  for (int i = 0; i < n; i++)
    ids[i] = thread_create("spin", spinner, &counters[i]);
  thread_sleep(30);
  stop = 1;
  for (int i = 0; i < n; i++)
    thread_join(ids[i]);
  int all_ran = 1;
  for (int i = 0; i < n; i++)
    all_ran &= counters[i] != 0;
  check("busy threads are preempted and all make progress", all_ran);
  check("threads ran on every CPU", cpus_seen == (1u << cpus) - 1);
  kfree(ids);
  kfree(counters);

  check("thread_exit code reaches join", thread_join(thread_create("exit7", exits_seven, 0)) == 7);

  run_all(quick, MANY_THREADS);
  check("hundreds of threads at once", many_done == MANY_THREADS);

  run_all(locked_incrementer, cpus);
  check("spinlock gives mutual exclusion across CPUs", shared_counter == (uint32_t)cpus * LOCK_ROUNDS);

  run_all(allocator, cpus);
  check("concurrent pmm and heap use stays consistent", alloc_errors == 0);

  struct user_image ia, ib, ic, id;
  int loaded = !user_image_code(&ia, user_hello_start, (uint32_t)(user_hello_end - user_hello_start)) &&
               !user_image_code(&ib, user_fault_start, (uint32_t)(user_fault_end - user_fault_start)) &&
               !elf_load(&ic, ELF(hello)) && !elf_load(&id, ELF(hello));
  check("user images built", loaded);
  int a = thread_create_user("hello1", &ia), b = thread_create_user("fault", &ib);
  int c = thread_create_user("elf1", &ic), d = thread_create_user("elf2", &id);
  int ra = thread_join(a), rb = thread_join(b), rc = thread_join(c), rd = thread_join(d);
  check("concurrent EL0 tasks keep separate address spaces and exit codes", ra == 42 && rb == -1 && rc == 0 && rd == 0);

  check("scheduler releases frames and stacks", pmm_free_kib() == frames && heap_free_bytes() == heap);
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
  sched_init();

  gic_init();
  timer_init();
  __asm__ volatile("msr daifclr, #2");
  while (timer_ticks() < 10)
    __asm__ volatile("wfi");
  check("timer interrupts via GIC", timer_ticks() >= 10);

  test_user();
  test_elf();

  smp_start();
  test_sched();

  uart_puts(failures ? "AARCH64 SELFTEST FAIL\n" : "AARCH64 SELFTEST PASS\n");
  arch_poweroff();
}
