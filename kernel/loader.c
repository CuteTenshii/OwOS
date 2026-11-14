/* loader.c: set up the user region, parse an ELF executable (ELF32 on i386, ELF64 on x86_64 and AArch64), and run it.
 */

#include "loader.h"
#include "elf.h"
#include "irqflags.h"
#include "kstring.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"

/* The scratch mapping is shared: each map/unmap interval runs with interrupts off (irq_save). */

/* Map [vaddr, vaddr+memsz) from fresh frames: copy filesz bytes, zero the rest. 0 or -1 if out of memory. */
static int load_segment(pgdir_t *dir, uaddr_t vaddr, const uint8_t *src, uint32_t filesz, uint32_t memsz)
{
  uaddr_t first = vaddr & ~(uaddr_t)0xFFF;
  uaddr_t last = (vaddr + memsz + 0xFFF) & ~(uaddr_t)0xFFF;
  for (uaddr_t page = first; page < last; page += 4096) {
    /* A page another segment already mapped is shared: write only this segment's bytes into it. */
    uint32_t phys = paging_user_phys(dir, page);
    bool fresh = phys == 0;
    if (fresh && !(phys = (uint32_t)pmm_alloc_frame())) /* frames lie below 4 GiB on every target */
      return -1;
    uintptr_t fl = irq_save();
    uint8_t *kv = paging_scratch_map(0, phys);
    /* This segment covers [lo,top) of the page: file data in [lo,hi), .bss after it. */
    uaddr_t seg_end = vaddr + filesz, mem_end = vaddr + memsz;
    uint32_t lo = (vaddr > page) ? (uint32_t)(vaddr - page) : 0;
    uint32_t hi = (seg_end > page) ? (uint32_t)(seg_end - page) : 0;
    uint32_t top = (mem_end > page) ? (uint32_t)(mem_end - page) : 0;
    if (hi > 4096)
      hi = 4096;
    if (top > 4096)
      top = 4096;
    if (hi < lo)
      hi = lo;
    if (top < hi)
      top = hi;
    if (fresh)
      memset(kv, 0, 4096);
    if (hi > lo)
      memcpy(kv + lo, src + page + lo - vaddr, hi - lo);
    if (top > hi)
      memset(kv + hi, 0, top - hi);
    paging_scratch_unmap(0);
    irq_restore(fl);
    if (fresh && paging_map_user_at(dir, page, phys, 1) < 0) {
      pmm_free_frame(phys);
      return -1;
    }
  }
  return 0;
}

/* User programs match the kernel's architecture and word size: the x86_64 kernel runs only 64-bit ones. */
#if defined(__aarch64__)
typedef Elf64_Ehdr elf_ehdr;
typedef Elf64_Phdr elf_phdr;
#define ELF_CLASS ELFCLASS64
#define ELF_MACHINE EM_AARCH64
#elif defined(__x86_64__)
typedef Elf64_Ehdr elf_ehdr;
typedef Elf64_Phdr elf_phdr;
#define ELF_CLASS ELFCLASS64
#define ELF_MACHINE EM_X86_64
#else
typedef Elf32_Ehdr elf_ehdr;
typedef Elf32_Phdr elf_phdr;
#define ELF_CLASS ELFCLASS32
#define ELF_MACHINE EM_386
#endif

/* A segment must stay below the stack and thread-stack region, whose pages load_elf and SYS_STACK_ALLOC map over
 * whatever is there. Taken at 64 bits so no ELF64 field can wrap past the check. */
static bool in_image_window(uint64_t addr, uint64_t len)
{
  return addr >= USER_BASE && len <= USER_HEAP_MAX && addr <= USER_HEAP_MAX - len;
}

int load_elf(const void *image, uint32_t size, pgdir_t **out_dir, uaddr_t *out_entry, uaddr_t *out_brk)
{
  const uint8_t *file = image;
  const elf_ehdr *eh = image;
  uaddr_t image_end = USER_BASE; /* highest vaddr used by any segment */

  if (size < sizeof(elf_ehdr))
    return -1;
  if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
    return -1;
  if (eh->e_ident[EI_CLASS] != ELF_CLASS || eh->e_machine != ELF_MACHINE || eh->e_type != ET_EXEC)
    return -1;
#ifdef __x86_64__
  /* The entry is handed on as a 32-bit word; a higher one would be cut short rather than refused. */
  if (eh->e_entry > 0xFFFFFFFFu)
    return -1;
#endif

  /* Validate the untrusted phdr table (64-bit math avoids wrap) before indexing it. */
  if (eh->e_phentsize < sizeof(elf_phdr) || eh->e_phoff > size ||
      (uint64_t)eh->e_phnum * eh->e_phentsize > size - (uint64_t)eh->e_phoff)
    return -1;

  pgdir_t *dir = paging_new_process_dir();
  if (!dir)
    return -1;

  for (uint32_t i = 0; i < eh->e_phnum; i++) {
    const elf_phdr *ph = (const elf_phdr *)(file + eh->e_phoff + i * eh->e_phentsize);
    if (ph->p_type != PT_LOAD)
      continue;
    if (!in_image_window(ph->p_vaddr, ph->p_memsz) || ph->p_filesz > size || ph->p_offset > size - ph->p_filesz ||
        load_segment(dir, (uaddr_t)ph->p_vaddr, file + ph->p_offset, (uint32_t)ph->p_filesz, (uint32_t)ph->p_memsz) !=
            0) {
      paging_free_process_dir(dir);
      return -1;
    }
    if (ph->p_vaddr + ph->p_memsz > image_end)
      image_end = (uaddr_t)(ph->p_vaddr + ph->p_memsz);
  }

  for (uint32_t p = 0; p < USER_STACK_PAGES; p++) {
    uint32_t phys = (uint32_t)pmm_alloc_frame();
    if (!phys) {
      paging_free_process_dir(dir);
      return -1;
    }
    uintptr_t fl = irq_save();
    uint32_t *kv = paging_scratch_map(0, phys);
    memset(kv, 0, 4096);
    paging_scratch_unmap(0);
    irq_restore(fl);
    if (paging_map_user_at(dir, USER_STACK_TOP - (uaddr_t)(p + 1) * 4096, phys, 1) < 0) {
      pmm_free_frame(phys);
      paging_free_process_dir(dir);
      return -1;
    }
  }

  *out_dir = dir;
  *out_entry = (uaddr_t)eh->e_entry;
  *out_brk = (image_end + 0xFFF) & ~(uaddr_t)0xFFF;
  return 0;
}

#define MAX_ARGS 16            /* must be >= SYS_SPAWN_CAP's MAXARGS (syscall.c) */
#define ARG_TOP USER_STACK_TOP /* args live in the top mapped stack page    */

uaddr_t setup_args(pgdir_t *dir, int argc, char *const argv[])
{
  /* The top stack page holds the args, written via the scratch map (IF off); pointers are user vaddrs, each one
   * user word (the kernel's word size) wide. */
  uaddr_t page = ARG_TOP - 4096;
  uint32_t pa = paging_user_phys(dir, page);
  if (!pa)
    return USER_STACK_TOP;
  if (argc > MAX_ARGS)
    argc = MAX_ARGS;

  uintptr_t fl = irq_save();
  uint8_t *fp = paging_scratch_map(0, pa);
  uintptr_t uargv[MAX_ARGS];
  uaddr_t sp = ARG_TOP;

  /* Keep strings above room for argv[] (argc+1 pointers), argc and the 16-byte alignment below (under 32 bytes),
   * so the PUSHes never underflow. */
  const uaddr_t floor = page + (uaddr_t)(MAX_ARGS + 2) * sizeof(uintptr_t) + 32;

  for (int i = 0; i < argc; i++) {
    uint32_t len = (uint32_t)strlen(argv[i]) + 1;
    /* Out of room: keep the args that fit. */
    if (sp - len < floor) {
      argc = i;
      break;
    }
    sp -= len;
    for (uint32_t k = 0; k < len; k++)
      fp[sp - page + k] = (uint8_t)argv[i][k];
    uargv[i] = sp;
  }
  /* argc lands 16-byte aligned: the x86_64 and AArch64 ABIs' entry state, and harmless on i386. */
  sp &= ~(uaddr_t)15;
  sp -= (16u - (uint32_t)(argc + 2) * sizeof(uintptr_t) % 16u) % 16u;

#define PUSH(val)                                                                                                      \
  do {                                                                                                                 \
    sp -= sizeof(uintptr_t);                                                                                           \
    *(uintptr_t *)(fp + (sp - page)) = (val);                                                                          \
  } while (0)
  PUSH(0);
  for (int i = argc - 1; i >= 0; i--)
    PUSH(uargv[i]);
  PUSH((uintptr_t)argc);
#undef PUSH
  paging_scratch_unmap(0);
  irq_restore(fl);
  return sp;
}

static int spawn_image(const void *image, uint32_t size, int argc, char *const argv[], bool held)
{
  pgdir_t *dir;
  uaddr_t entry, brk;
  if (load_elf(image, size, &dir, &entry, &brk) != 0)
    return -1;

  uaddr_t user_esp = setup_args(dir, argc, argv);
  const char *nm = (argc > 0 && argv[0]) ? argv[0] : "user";
  int pid = sched_create_process(nm, entry, dir, user_esp, brk, held);
  if (pid < 0)
    paging_free_process_dir(dir);
  return pid;
}

int exec_program(const void *image, uint32_t size, int argc, char *const argv[])
{
  return spawn_image(image, size, argc, argv, false);
}

int exec_program_held(const void *image, uint32_t size, int argc, char *const argv[])
{
  return spawn_image(image, size, argc, argv, true);
}

/* Guarded thread stacks (SYS_STACK_ALLOC/FREE): per-CR3 slot bitmap; free keeps the frames. */
#define TSTACK_OWNERS 8
static struct {
  pgdir_t *dir;
  uint32_t used;
} tstack_tab[TSTACK_OWNERS];

static uint32_t *tstack_mask_for(pgdir_t *dir)
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
  return 0; /* table full */
}

static void tstack_zero(uint32_t phys)
{
  uintptr_t fl = irq_save();
  uint32_t *kv = paging_scratch_map(0, phys);
  for (int k = 0; k < 1024; k++)
    kv[k] = 0;
  paging_scratch_unmap(0);
  irq_restore(fl);
}

uaddr_t user_stack_alloc(int pages)
{
  pgdir_t *dir = sched_current_dir();
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
    return 0;
  uaddr_t top = TSTACK_TOP - (uaddr_t)slot * TSTACK_SLOT_PAGES * 4096u;
  for (int p = 0; p < pages; p++) {
    uaddr_t va = top - (uaddr_t)(p + 1) * 4096u;
    uint32_t phys = paging_user_phys(dir, va);
    if (!phys) {
      phys = (uint32_t)pmm_alloc_frame();
      if (!phys)
        return 0;
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

void user_stack_free(uaddr_t top)
{
  uint32_t *mask = tstack_mask_for(sched_current_dir());
  uaddr_t off, slot;
  if (!mask || top <= TSTACK_LOW || top > TSTACK_TOP)
    return;
  off = TSTACK_TOP - top;
  if (off % (TSTACK_SLOT_PAGES * 4096u))
    return;
  slot = off / (TSTACK_SLOT_PAGES * 4096u);
  if (slot < TSTACK_SLOTS)
    *mask &= ~(1u << slot);
}

void user_stack_reset_owner(pgdir_t *dir)
{
  for (int i = 0; i < TSTACK_OWNERS; i++)
    if (tstack_tab[i].dir == dir) {
      tstack_tab[i].dir = 0;
      tstack_tab[i].used = 0;
    }
}
