/* a64_elf_test.c: the AArch64 ELF loader (kernel/arch/aarch64/elf.c) against a fake page-table layer. Valid files
 * must map every page with the right permissions and contents; every malformed or hostile file must be refused
 * without leaking a frame, a mapping or an address space. */
#define OWOS_SUITE_TEST
#include "../kernel/arch/aarch64/uvm.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

#define PAGE 4096ULL
#define REGION_BASE (1ULL << 20)
#define REGION_SIZE (4ULL << 20)

static uint8_t *arena;

void *a64_host_ptr(uint64_t phys) { return arena + phys; }

/* ---- fake translation tables: a flat record of mappings per root ---- */
struct mapping {
  uint64_t *root;
  uint64_t va, pa;
  int prot;
};

#define MAX_MAPS 4096
static struct mapping maps[MAX_MAPS];
static int nmaps, live_roots, syncs, map_calls;
static int fail_create, fail_map_at; /* fail the Nth vm_map call from now (1-based), 0 = never */

uint64_t kernel_l1[512];

uint64_t *vm_create(void)
{
  if (fail_create)
    return 0;
  live_roots++;
  return calloc(1, sizeof(uint64_t));
}

int vm_map(uint64_t *root, uintptr_t va, uintptr_t pa, int prot)
{
  map_calls++;
  if (fail_map_at && --fail_map_at == 0)
    return -1;
  if (va < USER_BASE || va >= USER_END || nmaps == MAX_MAPS)
    return -1;
  maps[nmaps++] = (struct mapping){root, va, pa, prot};
  return 0;
}

void vm_switch(uint64_t *root) { (void)root; }

void vm_destroy(uint64_t *root)
{
  int kept = 0;
  for (int i = 0; i < nmaps; i++) {
    if (maps[i].root == root)
      pmm_free_frame(maps[i].pa);
    else
      maps[kept++] = maps[i];
  }
  nmaps = kept;
  live_roots--;
  free(root);
}

void vm_sync_code(phys_t frame)
{
  (void)frame;
  syncs++;
}

static const struct mapping *find(const struct user_image *img, uint64_t va)
{
  for (int i = 0; i < nmaps; i++)
    if (maps[i].root == img->root && maps[i].va == va)
      return &maps[i];
  return 0;
}

static void fresh(uint64_t frames)
{
  free(arena);
  arena = malloc(REGION_BASE + REGION_SIZE);
  memset(arena, 0xa5, REGION_BASE + REGION_SIZE); /* frames arrive dirty, as after earlier use */
  pmm_begin(REGION_BASE + REGION_SIZE, REGION_SIZE);
  pmm_add_region(REGION_BASE, frames * PAGE);
  pmm_reserve_low(REGION_BASE, REGION_BASE);
  nmaps = live_roots = syncs = map_calls = fail_create = fail_map_at = 0;
}

/* ---- ELF file builder ---- */
struct elf {
  uint8_t b[1 << 16];
  uint64_t size;
};

static void set(struct elf *e, uint64_t off, uint64_t v, int width) { memcpy(e->b + off, &v, (size_t)width); }

#define PH(i) (64 + 56 * (uint64_t)(i))

static void header(struct elf *e, uint64_t entry, int phnum)
{
  memset(e, 0, sizeof *e);
  memcpy(e->b,
         "\x7f"
         "ELF\2\1\1",
         7);
  set(e, 16, 2, 2);   /* ET_EXEC */
  set(e, 18, 183, 2); /* EM_AARCH64 */
  set(e, 20, 1, 4);
  set(e, 24, entry, 8);
  set(e, 32, 64, 8); /* e_phoff */
  set(e, 52, 64, 2);
  set(e, 54, 56, 2);
  set(e, 56, (uint64_t)phnum, 2);
  e->size = 64 + 56 * (uint64_t)phnum;
}

static void ph(struct elf *e, int i, uint32_t type, uint32_t flags, uint64_t off, uint64_t va, uint64_t filesz,
               uint64_t memsz)
{
  set(e, PH(i) + 0, type, 4);
  set(e, PH(i) + 4, flags, 4);
  set(e, PH(i) + 8, off, 8);
  set(e, PH(i) + 16, va, 8);
  set(e, PH(i) + 32, filesz, 8);
  set(e, PH(i) + 40, memsz, 8);
  set(e, PH(i) + 48, PAGE, 8);
}

enum { R = 4, W = 2, X = 1 };

static uint8_t pattern(uint64_t seed, uint64_t i) { return (uint8_t)(seed * 31 + i * 7 + (i >> 8) * 13 + 1); }

static void fill(struct elf *e, uint64_t off, uint64_t len, uint64_t seed)
{
  for (uint64_t i = 0; i < len; i++)
    e->b[off + i] = pattern(seed, i);
  if (off + len > e->size)
    e->size = off + len;
}

/* text 1.5 pages R+X, rodata 0x100 bytes R, data 0x200 bytes + bss up to 0x2200 R+W, plus a PT_NOTE to ignore. */
static void valid(struct elf *e)
{
  header(e, USER_BASE + 0x10, 4);
  ph(e, 0, 1, R | X, 0x1000, USER_BASE, 0x1800, 0x1800);
  ph(e, 1, 1, R, 0x3000, USER_BASE + 0x2000, 0x100, 0x100);
  ph(e, 2, 1, R | W, 0x3100, USER_BASE + 0x3000, 0x200, 0x2200);
  ph(e, 3, 4, R, 0x100, 0, 8, 8);
  fill(e, 0x1000, 0x1800, 1);
  fill(e, 0x3000, 0x100, 2);
  fill(e, 0x3100, 0x200, 3);
}

/* Page at `va` must have `prot`; its first `filled` bytes continue pattern(seed) from `first`, the rest is zero. */
static void expect_page(const struct user_image *img, uint64_t va, int prot, uint64_t seed, uint64_t first,
                        uint64_t filled)
{
  const struct mapping *m = find(img, va);
  expect(m != 0);
  if (!m)
    return;
  expect_eq(m->prot, prot);
  const uint8_t *p = a64_host_ptr(m->pa);
  int ok = 1;
  for (uint64_t i = 0; i < PAGE; i++)
    ok &= p[i] == (i < filled ? pattern(seed, first + i) : 0);
  expect(ok);
}

static void check_valid_file(void)
{
  fresh(1024);
  uint32_t before = pmm_free_kib();
  struct elf e;
  struct user_image img;
  valid(&e);
  expect_eq(elf_load(&img, e.b, e.size), 0);
  expect_eq(img.entry, USER_BASE + 0x10);
  expect_eq(img.sp, USER_END);

  expect_page(&img, USER_BASE, VM_USER_RX, 1, 0, PAGE);
  expect_page(&img, USER_BASE + 0x1000, VM_USER_RX, 1, PAGE, 0x800); /* half a page of text, then zero */
  expect_page(&img, USER_BASE + 0x2000, VM_USER_RO, 2, 0, 0x100);
  expect_page(&img, USER_BASE + 0x3000, VM_USER_RW, 3, 0, 0x200);
  expect_page(&img, USER_BASE + 0x4000, VM_USER_RW, 0, 0, 0); /* bss */
  expect_page(&img, USER_BASE + 0x5000, VM_USER_RW, 0, 0, 0);
  expect(!find(&img, USER_BASE + 0x6000)); /* nothing mapped past the segments */
  expect(!find(&img, USER_BASE + 0x1000 * 0xfff));
  for (uint64_t va = USER_STACK_BOTTOM; va < USER_END; va += PAGE)
    expect_page(&img, va, VM_USER_RW, 0, 0, 0);
  expect(!find(&img, USER_STACK_BOTTOM - PAGE));
  expect_eq(nmaps, 2 + 1 + 3 + USER_STACK_PAGES);
  expect_eq(syncs, 2); /* only code pages are synchronised for instruction fetch */

  vm_destroy(img.root);
  expect_eq(live_roots, 0);
  expect_eq(pmm_free_kib(), before);
}

/* Refused files leave nothing behind. */
static void expect_rejected(const struct elf *e, uint64_t size)
{
  uint32_t before = pmm_free_kib();
  struct user_image img;
  expect_eq(elf_load(&img, e->b, size), -1);
  expect_eq(live_roots, 0);
  expect_eq(nmaps, 0);
  expect_eq(pmm_free_kib(), before);
}

static void check_header_rejects(void)
{
  fresh(1024);
  struct elf e;
  for (int byte = 0; byte < 7; byte++) { /* magic, class, endianness, version */
    valid(&e);
    e.b[byte] ^= 0xff;
    expect_rejected(&e, e.size);
  }
  valid(&e);
  set(&e, 16, 3, 2); /* ET_DYN */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, 18, 62, 2); /* x86-64 */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, 54, 32, 2); /* wrong program header size */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, 56, 0, 2);
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, 56, 17, 2);
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, 32, e.size, 8); /* program headers start at EOF */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, 32, ~0ULL - 8, 8);
  expect_rejected(&e, e.size);
  valid(&e);
  expect_rejected(&e, 30);       /* shorter than the ELF header */
  expect_rejected(&e, 64 + 100); /* program header table cut short */
}

static void check_segment_rejects(void)
{
  fresh(1024);
  struct elf e;
  valid(&e);
  expect_rejected(&e, 0x3100); /* data segment's file bytes run past EOF */

  valid(&e);
  set(&e, PH(2) + 8, ~0ULL - 0x100, 8); /* offset + filesz overflows */
  expect_rejected(&e, e.size);
  valid(&e);
  e.size = 0x9000;                /* plenty of file, so only the size relation can refuse it */
  set(&e, PH(2) + 32, 0x3000, 8); /* filesz > memsz */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, PH(0) + 4, R | W | X, 4);
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, PH(0) + 16, 0x1000, 8); /* below the user window */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, PH(0) + 16, USER_BASE + 8, 8); /* not page aligned */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, PH(2) + 16, USER_STACK_BOTTOM, 8); /* starts inside the stack */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, PH(2) + 16, USER_STACK_BOTTOM - PAGE, 8);
  set(&e, PH(2) + 40, 2 * PAGE, 8); /* one page of data, one page inside the stack */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, PH(2) + 16, USER_END - PAGE, 8);
  set(&e, PH(2) + 40, ~0ULL - PAGE, 8); /* vaddr + memsz wraps */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, PH(1) + 16, USER_BASE + 0x1000, 8); /* shares a page with the text segment */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, PH(0) + 16, USER_BASE + 0x8000, 8); /* segments out of ascending order */
  set(&e, 24, USER_BASE + 0x8010, 8);
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, 24, USER_BASE + 0x2000, 8); /* entry inside the read-only segment */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, 24, 0, 8);
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, 24, USER_BASE + 0x1800, 8); /* entry just past the end of text */
  expect_rejected(&e, e.size);
  valid(&e);
  set(&e, PH(0), 4, 4); /* no PT_LOAD with code at all */
  expect_rejected(&e, e.size);
}

static void check_memory_limits(void)
{
  struct elf e;
  struct user_image img;

  fresh(256); /* 256 free frames: 64 for the stack leaves 192 for the file */
  header(&e, USER_BASE, 1);
  ph(&e, 0, 1, R | X, 0x1000, USER_BASE, 0x100, 200 * PAGE);
  fill(&e, 0x1000, 0x100, 1);
  expect_rejected(&e, e.size);
  expect_eq(map_calls, 0); /* refused before mapping a single page */

  ph(&e, 0, 1, R | X, 0x1000, USER_BASE, 0x100, 190 * PAGE);
  expect_eq(elf_load(&img, e.b, e.size), 0);
  expect_eq(pmm_free_kib(), (256 - 190 - USER_STACK_PAGES) * 4);
  vm_destroy(img.root);
  expect_eq(pmm_free_kib(), 256 * 4);

  fresh(20); /* fewer frames than the stack alone */
  valid(&e);
  expect_rejected(&e, e.size);

  fresh(1024);
  valid(&e);
  header(&e, USER_BASE, 1);
  ph(&e, 0, 1, R | X, 0x1000, USER_BASE, 0x100, 1ULL << 38); /* terabyte-scale memsz inside the window */
  expect_rejected(&e, e.size);
}

static void check_failures_unwind(void)
{
  struct elf e;
  struct user_image img;
  fresh(1024);
  valid(&e);
  uint32_t before = pmm_free_kib();

  fail_create = 1;
  expect_eq(elf_load(&img, e.b, e.size), -1);
  fail_create = 0;

  int total_maps = 2 + 1 + 3 + USER_STACK_PAGES;
  for (int n = 1; n <= total_maps; n++) { /* the Nth page-table insertion fails: text, rodata, data, then stack */
    fail_map_at = n;
    expect_eq(elf_load(&img, e.b, e.size), -1);
    expect_eq(live_roots, 0);
    expect_eq(nmaps, 0);
    expect_eq(pmm_free_kib(), before);
  }
  fail_map_at = 0;
  expect_eq(elf_load(&img, e.b, e.size), 0); /* and it still loads afterwards */
  vm_destroy(img.root);
  expect_eq(pmm_free_kib(), before);
}

static void check_flat_code(void)
{
  struct user_image img;
  uint8_t code[100];
  for (int i = 0; i < 100; i++)
    code[i] = (uint8_t)(i + 5);

  fresh(256);
  uint32_t before = pmm_free_kib();
  expect_eq(user_image_code(&img, code, sizeof code), 0);
  expect_eq(img.entry, USER_BASE);
  expect_eq(img.sp, USER_END);
  const struct mapping *m = find(&img, USER_BASE);
  expect(m && m->prot == VM_USER_RX && !memcmp(a64_host_ptr(m->pa), code, sizeof code));
  expect_eq(nmaps, 1 + USER_STACK_PAGES);
  vm_destroy(img.root);
  expect_eq(pmm_free_kib(), before);

  expect_eq(user_image_code(&img, code, PAGE + 1), -1); /* does not fit one page */
  expect_eq(live_roots, 0);
  expect_eq(pmm_free_kib(), before);
}

int main(void)
{
  test("valid file: permissions, contents, bss, stack", check_valid_file);
  test("header and program header table rejects", check_header_rejects);
  test("segment layout rejects", check_segment_rejects);
  test("segments larger than free memory", check_memory_limits);
  test("failures at every mapping unwind completely", check_failures_unwind);
  test("flat code images", check_flat_code);
  return 0;
}
