/* loader.c — set up the user region, parse an ELF32 executable, and run it. */

#include "loader.h"
#include "paging.h"
#include "gdt.h"
#include "elf.h"
#include "sched.h"

#define USER_STACK_PAGES 4   /* 16 KiB of user stack                          */

/* Map [vaddr, vaddr+memsz) into dir, copying filesz bytes of segment data
 * from `src` and zeroing the rest (.bss). Returns 0 on success. */
static int load_segment(uint32_t *dir, uint32_t vaddr, const uint8_t *src,
                        uint32_t filesz, uint32_t memsz)
{
	uint32_t first = vaddr & ~0xFFFu;
	uint32_t last  = (vaddr + memsz + 0xFFFu) & ~0xFFFu;
	for (uint32_t page = first; page < last; page += 4096) {
		uint8_t *frame = palloc();
		if (!frame)
			return -1;
		for (uint32_t b = 0; b < 4096; b++) {
			uint32_t off = (page + b) - vaddr;   /* offset within the segment */
			frame[b] = (off < filesz) ? src[off] : 0;
		}
		paging_map_user_page(dir, page, (uint32_t)frame);
	}
	return 0;
}

/* A user address range is valid only if it sits entirely in the user window. */
static int in_user_window(uint32_t addr, uint32_t len)
{
	return addr >= USER_BASE
	    && addr + len <= USER_REGION_END
	    && addr + len >= addr;
}

int load_elf(const void *image, uint32_t size, uint32_t **out_dir, uint32_t *out_entry)
{
	const uint8_t *file = image;
	const Elf32_Ehdr *eh = image;

	if (size < sizeof(Elf32_Ehdr))
		return -1;
	if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
		return -1;
	if (eh->e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh->e_machine != EM_386 || eh->e_type != ET_EXEC)
		return -1;

	/* A fresh address space: kernel mappings shared, user window private. */
	uint32_t *dir = paging_new_process_dir();
	if (!dir)
		return -1;

	for (int i = 0; i < eh->e_phnum; i++) {
		const Elf32_Phdr *ph =
			(const Elf32_Phdr *)(file + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
		if (ph->p_type != PT_LOAD)
			continue;
		if (!in_user_window(ph->p_vaddr, ph->p_memsz) ||
		    ph->p_offset + ph->p_filesz > size ||
		    load_segment(dir, ph->p_vaddr, file + ph->p_offset,
		                 ph->p_filesz, ph->p_memsz) != 0) {
			paging_free_process_dir(dir);
			return -1;
		}
	}

	for (int p = 0; p < USER_STACK_PAGES; p++) {
		uint8_t *frame = palloc();
		if (!frame) {
			paging_free_process_dir(dir);
			return -1;
		}
		paging_map_user_page(dir, USER_STACK_TOP - (uint32_t)(p + 1) * 4096,
		                     (uint32_t)frame);
	}

	*out_dir = dir;
	*out_entry = eh->e_entry;
	return 0;
}

#define MAX_ARGS 8
#define ARG_TOP  0x003FF000u   /* args live in the top mapped stack page       */

static int str_len(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

uint32_t setup_args(uint32_t *dir, int argc, char *const argv[])
{
	/* The top stack page [ARG_TOP-4K, ARG_TOP) backs the argument block; we
	 * write through its physical frame (identity-mapped) but store pointers
	 * as the user virtual addresses the process will see. */
	uint32_t page = ARG_TOP - 4096;
	uint8_t *fp = (uint8_t *)paging_user_phys(dir, page);
	if (!fp || argc > MAX_ARGS)
		return USER_STACK_TOP;

	uint32_t uargv[MAX_ARGS];
	uint32_t sp = ARG_TOP;

	for (int i = 0; i < argc; i++) {           /* copy the strings            */
		int len = str_len(argv[i]) + 1;
		sp -= (uint32_t)len;
		if (sp < page)
			return USER_STACK_TOP;             /* args too big; bail safely   */
		for (int k = 0; k < len; k++)
			fp[sp - page + (uint32_t)k] = (uint8_t)argv[i][k];
		uargv[i] = sp;
	}
	sp &= ~3u;

#define PUSH(val) do { sp -= 4; *(uint32_t *)(fp + (sp - page)) = (val); } while (0)
	PUSH(0);                                   /* argv[argc] = NULL           */
	for (int i = argc - 1; i >= 0; i--)
		PUSH(uargv[i]);                        /* argv[i]                     */
	PUSH((uint32_t)argc);                      /* argc, at the lowest addr    */
#undef PUSH
	return sp;
}

int exec_program(const void *image, uint32_t size, int argc, char *const argv[])
{
	uint32_t *dir;
	uint32_t  entry;
	if (load_elf(image, size, &dir, &entry) != 0)
		return -1;

	uint32_t user_esp = setup_args(dir, argc, argv);
	int pid = sched_create_process("user", entry, dir, user_esp);
	if (pid < 0)
		paging_free_process_dir(dir);
	return pid;
}
