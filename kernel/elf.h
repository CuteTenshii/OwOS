/* elf.h: the bits of ELF we need to load a static executable (ELF32 on i386, ELF64 on x86_64). */
#ifndef OWOS_ELF_H
#define OWOS_ELF_H

#include <stdint.h>

typedef struct {
  uint8_t e_ident[16]; /* magic + class/data/version                     */
  uint16_t e_type;     /* ET_EXEC for a static executable                */
  uint16_t e_machine;  /* EM_386                                         */
  uint32_t e_version;
  uint32_t e_entry; /* virtual address of the entry point             */
  uint32_t e_phoff; /* program header table offset                    */
  uint32_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize; /* size of one program header                     */
  uint16_t e_phnum;     /* number of program headers                      */
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  uint32_t p_type;   /* PT_LOAD entries are the loadable segments      */
  uint32_t p_offset; /* offset of the segment within the file          */
  uint32_t p_vaddr;  /* virtual address to load it at                  */
  uint32_t p_paddr;
  uint32_t p_filesz; /* bytes present in the file                      */
  uint32_t p_memsz;  /* bytes in memory (>= filesz; the rest is .bss)  */
  uint32_t p_flags;
  uint32_t p_align;
} Elf32_Phdr;

/* The same fields at 64-bit width; p_flags moves up beside p_type. */
typedef struct {
  uint8_t e_ident[16];
  uint16_t e_type;
  uint16_t e_machine; /* EM_X86_64 */
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} Elf64_Phdr;

#define EI_CLASS 4
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ET_EXEC 2
#define EM_386 3
#define EM_X86_64 62
#define PT_LOAD 1

#endif /* OWOS_ELF_H */
