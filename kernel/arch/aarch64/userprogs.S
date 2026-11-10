/* userprogs.S: the AArch64 user programs, embedded as ELF files. */

.section .rodata
.balign 16

.global hello_elf_start, hello_elf_end
hello_elf_start:
	.incbin	"user/aarch64/hello.elf"
hello_elf_end:
.balign 16

.global segv_elf_start, segv_elf_end
segv_elf_start:
	.incbin	"user/aarch64/segv.elf"
segv_elf_end:
.balign 16

.global nx_elf_start, nx_elf_end
nx_elf_start:
	.incbin	"user/aarch64/nx.elf"
nx_elf_end:
.balign 16
