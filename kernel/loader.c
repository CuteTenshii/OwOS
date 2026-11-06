/* loader.c: set up the user region, parse an ELF32 executable, and run it. */

#include "loader.h"
#include "elf.h"
#include "gdt.h"
#include "irqflags.h"
#include "kstring.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"

/* The scratch mapping is shared: each map/unmap interval runs with interrupts off (irq_save). */

/* Map [vaddr, vaddr+memsz) from fresh frames: copy filesz bytes, zero the rest. 0 or -1 if out of memory. */
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
    /* Page = [0,lo) padding, [lo,hi) file data, [hi,4096) .bss tail. */
    uint32_t seg_end = vaddr + filesz;
    uint32_t lo = (vaddr > page) ? (vaddr - page) : 0;
    uint32_t hi = (seg_end > page) ? (seg_end - page) : 0;
    if (hi > 4096)
      hi = 4096;
    if (hi < lo)
      hi = lo;
    if (lo)
      memset(kv, 0, lo);
    if (hi > lo)
      memcpy(kv + lo, src + page + lo - vaddr, hi - lo);
    if (hi < 4096)
      memset(kv + hi, 0, 4096 - hi);
    paging_scratch_unmap(0);
    irq_restore(fl);
    if (paging_map_user_at(dir, page, phys, 1) < 0) {
      pmm_free_frame(phys);
      return -1;
    }
  }
  return 0;
}

/* A user address range is valid only if it sits entirely in the user window. */
static bool in_user_window(uint32_t addr, uint32_t len)
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

  uint32_t *dir = paging_new_process_dir();
  if (!dir)
    return -1;

  /* Validate the untrusted phdr table (64-bit math avoids wrap) before indexing it. */
  if (eh->e_phentsize < sizeof(Elf32_Phdr) || (uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
    paging_free_process_dir(dir);
    return -1;
  }

  for (uint32_t i = 0; i < eh->e_phnum; i++) {
    const Elf32_Phdr *ph = (const Elf32_Phdr *)(file + eh->e_phoff + i * eh->e_phentsize);
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

  for (uint32_t p = 0; p < USER_STACK_PAGES; p++) {
    uint32_t phys = pmm_alloc_frame();
    if (!phys) {
      paging_free_process_dir(dir);
      return -1;
    }
    uint32_t fl = irq_save();
    uint32_t *kv = paging_scratch_map(0, phys);
    memset(kv, 0, 4096);
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
  *out_brk = (image_end + 0xFFFu) & ~0xFFFu;
  return 0;
}

#define MAX_ARGS 16            /* must be >= SYS_SPAWN_CAP's MAXARGS (syscall.c) */
#define ARG_TOP USER_STACK_TOP /* args live in the top mapped stack page    */

uint32_t setup_args(uint32_t *dir, int argc, char *const argv[])
{
  /* The top stack page holds the args, written via the scratch map (IF off); pointers are user vaddrs. */
  uint32_t page = ARG_TOP - 4096;
  uint32_t pa = paging_user_phys(dir, page);
  if (!pa)
    return USER_STACK_TOP;
  if (argc > MAX_ARGS)
    argc = MAX_ARGS;

  uint32_t fl = irq_save();
  uint8_t *fp = paging_scratch_map(0, pa);
  uint32_t uargv[MAX_ARGS];
  uint32_t sp = ARG_TOP;

  /* Keep strings above room for argv[] (argc+1 pointers) and argc, so the PUSHes never underflow. */
  const uint32_t floor = page + (uint32_t)(MAX_ARGS + 2) * 4;

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
  sp &= ~3u;

#define PUSH(val)                                                                                                      \
  do {                                                                                                                 \
    sp -= 4;                                                                                                           \
    *(uint32_t *)(fp + (sp - page)) = (val);                                                                           \
  } while (0)
  PUSH(0);
  for (int i = argc - 1; i >= 0; i--)
    PUSH(uargv[i]);
  PUSH((uint32_t)argc);
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
  const char *nm = (argc > 0 && argv[0]) ? argv[0] : "user";
  int pid = sched_create_process(nm, entry, dir, user_esp, brk);
  if (pid < 0)
    paging_free_process_dir(dir);
  return pid;
}

/* Guarded thread stacks (SYS_STACK_ALLOC/FREE): per-CR3 slot bitmap; free keeps the frames. */
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
  return 0; /* table full */
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
    return 0;
  uint32_t top = TSTACK_TOP - (uint32_t)slot * TSTACK_SLOT_PAGES * 4096u;
  for (int p = 0; p < pages; p++) {
    uint32_t va = top - (uint32_t)(p + 1) * 4096u;
    uint32_t phys = paging_user_phys(dir, va);
    if (!phys) {
      phys = pmm_alloc_frame();
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

void user_stack_free(uint32_t top)
{
  uint32_t *mask = tstack_mask_for(sched_current_dir());
  uint32_t off, slot;
  if (!mask || top <= TSTACK_LOW || top > TSTACK_TOP)
    return;
  off = TSTACK_TOP - top;
  if (off % (TSTACK_SLOT_PAGES * 4096u))
    return;
  slot = off / (TSTACK_SLOT_PAGES * 4096u);
  if (slot < TSTACK_SLOTS)
    *mask &= ~(1u << slot);
}

void user_stack_reset_owner(uint32_t *dir)
{
  for (int i = 0; i < TSTACK_OWNERS; i++)
    if (tstack_tab[i].dir == dir) {
      tstack_tab[i].dir = 0;
      tstack_tab[i].used = 0;
    }
}
