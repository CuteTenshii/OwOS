/* elf.c: build user address spaces, from flat code or from ELF64 AArch64 executables. */
#include "arch.h"
#include "kstring.h"
#include <stdbool.h>

#define PAGE PMM_FRAME_SIZE
#define EM_AARCH64 183
#define ET_EXEC 2
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define MAX_PHDRS 16

struct elf64_hdr {
  uint8_t ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, phoff, shoff;
  uint32_t flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
  uint32_t type, flags;
  uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

static void sync_code(uintptr_t frame)
{
  for (uintptr_t a = frame; a < frame + PAGE; a += 64)
    __asm__ volatile("dc cvau, %0\n\tdsb ish\n\tic ivau, %0" : : "r"(a) : "memory");
  __asm__ volatile("dsb ish\n\tisb");
}

/* Map one zeroed page holding `len` bytes of `src` (src may be null) at `va`. */
static int map_page(uint64_t *root, uint64_t va, const void *src, uint64_t len, int prot)
{
  phys_t frame = pmm_alloc_frame();
  if (!frame)
    return -1;
  memset((void *)frame, 0, PAGE);
  if (len)
    memcpy((void *)frame, src, (uint32_t)len);
  if (prot == VM_USER_RX)
    sync_code(frame);
  if (vm_map(root, va, frame, prot)) {
    pmm_free_frame(frame);
    return -1;
  }
  return 0;
}

static int map_stack(uint64_t *root)
{
  for (uint64_t va = USER_STACK_BOTTOM; va < USER_END; va += PAGE)
    if (map_page(root, va, 0, 0, VM_USER_RW))
      return -1;
  return 0;
}

int user_image_code(struct user_image *img, const void *code, uint32_t len)
{
  uint64_t *root = vm_create();
  if (!root)
    return -1;
  if (len > PAGE || map_page(root, USER_BASE, code, len, VM_USER_RX) || map_stack(root)) {
    vm_destroy(root);
    return -1;
  }
  img->root = root;
  img->entry = USER_BASE;
  img->sp = USER_END;
  return 0;
}

static bool in_file(uint64_t size, uint64_t off, uint64_t len) { return off <= size && len <= size - off; }

int elf_load(struct user_image *img, const void *file, uint64_t size)
{
  const uint8_t *base = file;
  struct elf64_hdr h;
  if (size < sizeof h)
    return -1;
  memcpy(&h, base, sizeof h);
  static const uint8_t magic[] = {0x7f, 'E', 'L', 'F', 2, 1, 1}; /* ELF, 64-bit, little endian, version 1 */
  if (memcmp(h.ident, magic, sizeof magic) || h.type != ET_EXEC || h.machine != EM_AARCH64 ||
      h.phentsize != sizeof(struct elf64_phdr) || h.phnum == 0 || h.phnum > MAX_PHDRS ||
      !in_file(size, h.phoff, (uint64_t)h.phnum * h.phentsize))
    return -1;

  uint64_t *root = vm_create();
  if (!root)
    return -1;

  bool entry_ok = false;
  uint64_t free_pages = (uint64_t)pmm_free_kib() / (PAGE / 1024);
  uint64_t pages_left =
      free_pages > USER_STACK_PAGES ? free_pages - USER_STACK_PAGES : 0; /* refuse before exhausting RAM */
  uint64_t next_free = USER_BASE; /* segments must ascend and never share a page */
  for (unsigned i = 0; i < h.phnum; i++) {
    struct elf64_phdr p;
    memcpy(&p, base + h.phoff + (uint64_t)i * sizeof p, sizeof p);
    if (p.type != PT_LOAD || p.memsz == 0)
      continue;

    int prot = (p.flags & PF_X) ? VM_USER_RX : (p.flags & PF_W) ? VM_USER_RW : VM_USER_RO;
    if ((p.flags & PF_X) && (p.flags & PF_W))
      goto bad;
    if (p.filesz > p.memsz || !in_file(size, p.offset, p.filesz) || (p.vaddr & (PAGE - 1)) || p.vaddr < next_free ||
        p.vaddr >= USER_STACK_BOTTOM || p.memsz > USER_STACK_BOTTOM - p.vaddr)
      goto bad;

    uint64_t end = (p.vaddr + p.memsz + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    if ((end - p.vaddr) / PAGE > pages_left)
      goto bad;
    pages_left -= (end - p.vaddr) / PAGE;
    for (uint64_t va = p.vaddr; va < end; va += PAGE) {
      uint64_t done = va - p.vaddr;
      uint64_t chunk = done >= p.filesz ? 0 : p.filesz - done < PAGE ? p.filesz - done : PAGE;
      if (map_page(root, va, base + p.offset + done, chunk, prot))
        goto bad;
    }
    next_free = end;
    if ((p.flags & PF_X) && h.entry >= p.vaddr && h.entry < p.vaddr + p.memsz)
      entry_ok = true;
  }
  if (!entry_ok || map_stack(root))
    goto bad;

  img->root = root;
  img->entry = h.entry;
  img->sp = USER_END;
  return 0;

bad:
  vm_destroy(root);
  return -1;
}
