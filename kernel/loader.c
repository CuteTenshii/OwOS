/* loader.c — set up the user region, parse an ELF32 executable, and run it. */

#include "loader.h"
#include "elf.h"
#include "gdt.h"
#include "irqflags.h"
#include "kstring.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"

/* USER_STACK_PAGES lives in loader.h now (shared with the heap ceiling). */

/* The scratch mapping (paging_scratch_*) is shared kernel state, so a map->
 * write->unmap interval must not be preempted by another exec/fork using the
 * same slot. irq_save/irq_restore (irqflags.h) bracket each interval with
 * interrupts off and restore the prior IF, so it is safe whether exec runs from
 * a kernel thread (IF=1) or an int 0x80 syscall (IF=0). */

/* Map [vaddr, vaddr+memsz) into dir from fresh pmm frames, copying filesz bytes
 * of segment data and zeroing the rest (.bss). Each frame is written in full
 * (data then zero-fill), so no stale RAM from a previous process leaks through.
 * Returns 0 on success, -1 if physical memory is exhausted. */
static int load_segment(uint32_t *dir, uint32_t vaddr, const uint8_t *src, uint32_t filesz, uint32_t memsz)
{
  uint32_t first = vaddr & ~0xFFFu;
  uint32_t last = (vaddr + memsz + 0xFFFu) & ~0xFFFu;
  for (uint32_t page = first; page < last; page += 4096) {
    uint32_t phys = pmm_alloc_frame();
    if (!phys)
      return -1;
    uint32_t fl = irq_save();
    uint8_t *kv = paging_scratch_map(0, phys);
    /* Split the page into [0,lo) pre-segment padding, [lo,hi) file data, and
     * [hi,4096) .bss tail. A pure-.bss page (the common case for big zeroed
     * buffers) is lo==hi==0, so it skips straight to the word-store zero-fill
     * below — ~4x fewer stores than the old per-byte branch, which matters at
     * -O0 where a 11 MiB .bss otherwise stalls the launcher for seconds. */
    uint32_t seg_end = vaddr + filesz;
    uint32_t lo = (vaddr > page) ? (vaddr - page) : 0;
    uint32_t hi = (seg_end > page) ? (seg_end - page) : 0;
    if (hi > 4096)
      hi = 4096;
    if (hi < lo)
      hi = lo;
    uint32_t b = 0;
    for (; b < lo; b++) /* pad before the segment start  */
      kv[b] = 0;
    uint32_t off = page + b - vaddr; /* segment offset at byte b      */
    for (; b < hi; b++)              /* file data for this page       */
      kv[b] = src[off++];
    while (b < 4096 && (b & 3u)) /* align up, then word-fill zeros */
      kv[b++] = 0;
    for (; b + 4 <= 4096; b += 4)
      *(uint32_t *)(kv + b) = 0;
    paging_scratch_unmap(0);
    irq_restore(fl);
    if (paging_map_user_at(dir, page, phys, 1) < 0) { /* private tables   */
      pmm_free_frame(phys);
      return -1;
    }
  }
  return 0;
}

/* A user address range is valid only if it sits entirely in the user window. */
static int in_user_window(uint32_t addr, uint32_t len)
{
  return addr >= USER_BASE && addr + len <= USER_REGION_END && addr + len >= addr;
}

int load_elf(const void *image, uint32_t size, uint32_t **out_dir, uint32_t *out_entry, uint32_t *out_brk)
{
  const uint8_t *file = image;
  const Elf32_Ehdr *eh = image;
  uint32_t image_end = USER_BASE; /* highest vaddr used by any segment */

  if (size < sizeof(Elf32_Ehdr))
    return -1;
  if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
    return -1;
  if (eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_machine != EM_386 || eh->e_type != ET_EXEC)
    return -1;

  /* A fresh address space: kernel mappings shared, user window private. */
  uint32_t *dir = paging_new_process_dir();
  if (!dir)
    return -1;

  /* The program-header table is unvalidated user input; reject one whose
   * entries are undersized or that runs past the image before indexing it —
   * otherwise the phdr loop is a ring-0 out-of-bounds read. 64-bit math so the
   * offset+count product can't wrap. */
  if (eh->e_phentsize < sizeof(Elf32_Phdr) || (uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
    paging_free_process_dir(dir);
    return -1;
  }

  for (int i = 0; i < eh->e_phnum; i++) {
    const Elf32_Phdr *ph = (const Elf32_Phdr *)(file + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
    if (ph->p_type != PT_LOAD)
      continue;
    if (!in_user_window(ph->p_vaddr, ph->p_memsz) || (uint64_t)ph->p_offset + ph->p_filesz > size ||
        load_segment(dir, ph->p_vaddr, file + ph->p_offset, ph->p_filesz, ph->p_memsz) != 0) {
      paging_free_process_dir(dir);
      return -1;
    }
    if (ph->p_vaddr + ph->p_memsz > image_end)
      image_end = ph->p_vaddr + ph->p_memsz;
  }

  for (int p = 0; p < USER_STACK_PAGES; p++) {
    uint32_t phys = pmm_alloc_frame();
    if (!phys) {
      paging_free_process_dir(dir);
      return -1;
    }
    uint32_t fl = irq_save(); /* zero it: never leak prior data  */
    uint32_t *kv = paging_scratch_map(0, phys);
    for (int k = 0; k < 1024; k++)
      kv[k] = 0;
    paging_scratch_unmap(0);
    irq_restore(fl);
    if (paging_map_user_at(dir, USER_STACK_TOP - (uint32_t)(p + 1) * 4096, phys, 1) < 0) {
      pmm_free_frame(phys);
      paging_free_process_dir(dir);
      return -1;
    }
  }

  *out_dir = dir;
  *out_entry = eh->e_entry;
  *out_brk = (image_end + 0xFFFu) & ~0xFFFu; /* heap starts past the image */
  return 0;
}

#define MAX_ARGS 16            /* must be >= SYS_SPAWN_CAP's MAXARGS (syscall.c) */
#define ARG_TOP USER_STACK_TOP /* args live in the top mapped stack page    */

uint32_t setup_args(uint32_t *dir, int argc, char *const argv[])
{
  /* The top stack page [ARG_TOP-4K, ARG_TOP) backs the argument block. Its
   * frame is pmm-backed (above the identity map), so reach it through the
   * supervisor scratch mapping; pointers are stored as the user virtual
   * addresses the process will see. Interrupts off for the whole build, since
   * we hold the scratch slot across it. */
  uint32_t page = ARG_TOP - 4096;
  uint32_t pa = paging_user_phys(dir, page);
  if (!pa) /* no stack page (unrecoverable) */
    return USER_STACK_TOP;
  if (argc > MAX_ARGS) /* clamp; never fault on excess */
    argc = MAX_ARGS;

  uint32_t fl = irq_save();
  uint8_t *fp = paging_scratch_map(0, pa);
  uint32_t uargv[MAX_ARGS];
  uint32_t sp = ARG_TOP;

  /* Reserve room at the bottom of the page for the argv pointer vector that the
   * push phase writes below the strings: argc+1 pointers (NULL-terminated) plus
   * the argc word, at most (MAX_ARGS+2) dwords. Keeping the strings above this
   * floor guarantees the unguarded PUSHes below never underflow past `page`. */
  const uint32_t floor = page + (uint32_t)(MAX_ARGS + 2) * 4;

  for (int i = 0; i < argc; i++) { /* copy the strings            */
    uint32_t len = (uint32_t)strlen(argv[i]) + 1;
    /* Out of room in the arg page: keep the args that fit (argv[0..i)) and
     * stop, rather than returning an unmapped esp the program would fault on.
     */
    if (sp - len < floor) {
      argc = i;
      break;
    }
    sp -= len;
    for (uint32_t k = 0; k < len; k++)
      fp[sp - page + k] = (uint8_t)argv[i][k];
    uargv[i] = sp;
  }
  sp &= ~3u;

#define PUSH(val)                                                                                                      \
  do {                                                                                                                 \
    sp -= 4;                                                                                                           \
    *(uint32_t *)(fp + (sp - page)) = (val);                                                                           \
  } while (0)
  PUSH(0); /* argv[argc] = NULL           */
  for (int i = argc - 1; i >= 0; i--)
    PUSH(uargv[i]);     /* argv[i]                     */
  PUSH((uint32_t)argc); /* argc, at the lowest addr    */
#undef PUSH
  paging_scratch_unmap(0);
  irq_restore(fl);
  return sp;
}

int exec_program(const void *image, uint32_t size, int argc, char *const argv[])
{
  uint32_t *dir;
  uint32_t entry, brk;
  if (load_elf(image, size, &dir, &entry, &brk) != 0)
    return -1;

  uint32_t user_esp = setup_args(dir, argc, argv);
  /* Name the task after the program (argv[0]) so the crash reporter and
   * `tasks` show e.g. "browser" instead of a generic "user". */
  const char *nm = (argc > 0 && argv[0]) ? argv[0] : "user";
  int pid = sched_create_process(nm, entry, dir, user_esp, brk);
  if (pid < 0)
    paging_free_process_dir(dir);
  return pid;
}

/* ---- guarded thread stacks (SYS_STACK_ALLOC/FREE) ------------------------- *
 * A per-address-space (CR3) bitmap of the fixed slots in the [TSTACK_LOW,
 * TSTACK_TOP) region. Each slot maps its top `pages` frames and leaves the rest
 * (down to and including its lowest page) unmapped, so a thread stack overflow
 * faults. Freeing keeps the mapping and just clears the bit — the frames are
 * reused by the next alloc of that slot (bounded to 8 MiB/process) and
 * reclaimed with the address space on exit; a per-page unmap isn't available
 * (the range unmap is 2 MiB-granular and would clobber neighbouring slots). */
#define TSTACK_OWNERS 8
static struct {
  uint32_t *dir;
  uint32_t used;
} tstack_tab[TSTACK_OWNERS];

static uint32_t *tstack_mask_for(uint32_t *dir)
{
  for (int i = 0; i < TSTACK_OWNERS; i++)
    if (tstack_tab[i].dir == dir)
      return &tstack_tab[i].used;
  for (int i = 0; i < TSTACK_OWNERS; i++)
    if (!tstack_tab[i].dir) {
      tstack_tab[i].dir = dir;
      tstack_tab[i].used = 0;
      return &tstack_tab[i].used;
    }
  return 0; /* table full: caller falls back to a heap stack */
}

static void tstack_zero(uint32_t phys)
{
  uint32_t fl = irq_save();
  uint32_t *kv = paging_scratch_map(0, phys);
  for (int k = 0; k < 1024; k++)
    kv[k] = 0;
  paging_scratch_unmap(0);
  irq_restore(fl);
}

uint32_t user_stack_alloc(int pages)
{
  uint32_t *dir = sched_current_dir();
  uint32_t *mask;
  int slot = -1;
  if (pages <= 0 || pages > (int)(TSTACK_SLOT_PAGES - 1))
    return 0;
  mask = tstack_mask_for(dir);
  if (!mask)
    return 0;
  for (unsigned i = 0; i < TSTACK_SLOTS; i++)
    if (!(*mask & (1u << i))) {
      slot = (int)i;
      break;
    }
  if (slot < 0)
    return 0; /* all slots in use */
  uint32_t top = TSTACK_TOP - (uint32_t)slot * TSTACK_SLOT_PAGES * 4096u;
  for (int p = 0; p < pages; p++) {
    uint32_t va = top - (uint32_t)(p + 1) * 4096u;
    uint32_t phys = paging_user_phys(dir, va); /* reuse a prior mapping of this slot */
    if (!phys) {
      phys = pmm_alloc_frame();
      if (!phys)
        return 0; /* OOM: earlier pages of this slot leak until exit */
      if (paging_map_user_at(dir, va, phys, 1) < 0) {
        pmm_free_frame(phys);
        return 0;
      }
    }
    tstack_zero(phys);
  }
  *mask |= (1u << slot);
  return top;
}

void user_stack_free(uint32_t top)
{
  uint32_t *mask = tstack_mask_for(sched_current_dir());
  uint32_t off, slot;
  if (!mask || top <= TSTACK_LOW || top > TSTACK_TOP)
    return;
  off = TSTACK_TOP - top;
  if (off % (TSTACK_SLOT_PAGES * 4096u))
    return; /* not a slot top */
  slot = off / (TSTACK_SLOT_PAGES * 4096u);
  if (slot < TSTACK_SLOTS)
    *mask &= ~(1u << slot); /* keep the mapping for reuse */
}

void user_stack_reset_owner(uint32_t *dir)
{
  for (int i = 0; i < TSTACK_OWNERS; i++)
    if (tstack_tab[i].dir == dir) {
      tstack_tab[i].dir = 0;
      tstack_tab[i].used = 0;
    }
}
