#include "cpu_defs.h"

/* boot.S: AArch64 entry: park secondary cores, drop to EL1, clear BSS, enter C. */

.macro el2_to_el1 target
	mov	x0, #(1 << 31)               /* HCR_EL2.RW: EL1 is AArch64 */
	msr	hcr_el2, x0
	mov	x0, #0x33ff                  /* CPTR_EL2: no FP/SIMD traps */
	msr	cptr_el2, x0
	mrs	x0, cnthctl_el2
	orr	x0, x0, #3                   /* EL1 may use the physical counter and timer */
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr
	mrs	x0, id_aa64pfr0_el1
	ubfx	x0, x0, #24, #4
	cbz	x0, 1f                       /* no GICv3 system registers */
	mov	x0, #0xf                     /* ICC_SRE_EL2: SRE, DFB, DIB, Enable so EL1 can use them */
	msr	icc_sre_el2, x0
	isb
1:
	mov	x0, #0x3c5                   /* SPSR: EL1h, DAIF masked */
	msr	spsr_el2, x0
	adr	x0, \target
	msr	elr_el2, x0
	eret
.endm

/* Linux arm64 Image header: QEMU only passes the DTB in x0 to images that carry it. */
.section .text.boot
.global _start
_start:
	b	start_real
	.word	0
	.quad	0x80000                      /* text_offset */
	.quad	__image_size
	.quad	0                            /* flags: little endian */
	.quad	0, 0, 0
	.ascii	"ARM\x64"
	.word	0

start_real:
	mov	x20, x0                      /* DTB pointer from the loader */
	mrs	x0, mpidr_el1
	and	x0, x0, #0xff
	cbnz	x0, park

	/* The image is linked at 0, so the load address is the relocation delta. The build allows only RELATIVE and
	 * NONE (an empty slot ld leaves behind) entries, and nothing below has used an absolute pointer yet. */
	adr	x21, _start
	adrp	x0, __rela_start
	add	x0, x0, :lo12:__rela_start
	adrp	x1, __rela_end
	add	x1, x1, :lo12:__rela_end
	mov	w4, #R_AARCH64_RELATIVE
1:	cmp	x0, x1
	b.hs	2f
	ldp	x2, x3, [x0]                 /* r_offset, r_info */
	cmp	w3, w4
	b.ne	3f
	ldr	x3, [x0, #16]                /* r_addend */
	add	x3, x3, x21
	str	x3, [x2, x21]
3:	add	x0, x0, #24
	b	1b
2:

	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	adrp	x1, boot_el
	str	x0, [x1, :lo12:boot_el]
	cmp	x0, #2
	b.ne	at_el1

	el2_to_el1 at_el1

at_el1:
	adrp	x0, __stack_top
	add	x0, x0, :lo12:__stack_top
	mov	sp, x0

	mov	x0, #(3 << 20)               /* CPACR_EL1.FPEN: no FP/SIMD traps */
	msr	cpacr_el1, x0
	adrp	x0, vector_table
	add	x0, x0, :lo12:vector_table
	msr	vbar_el1, x0
	isb

	adrp	x0, __bss_start
	add	x0, x0, :lo12:__bss_start
	adrp	x1, __bss_end
	add	x1, x1, :lo12:__bss_end
1:	cmp	x0, x1
	b.hs	2f
	stp	xzr, xzr, [x0], #16
	b	1b
2:	adrp	x0, boot_dtb
	str	x20, [x0, :lo12:boot_dtb]
	bl	arch_main

park:	wfe
	b	park


/* Entered by PSCI CPU_ON with the MMU off: x0 = cpu index. */
.global secondary_entry
secondary_entry:
	mov	x20, x0
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	cmp	x0, #2
	b.ne	secondary_el1
	el2_to_el1 secondary_el1

secondary_el1:
	adrp	x0, secondary_stacks
	add	x0, x0, :lo12:secondary_stacks
	add	x1, x20, #1
	mov	x2, #SECONDARY_STACK_SIZE
	madd	x0, x1, x2, x0
	mov	sp, x0

	mov	x0, #(3 << 20)
	msr	cpacr_el1, x0
	adrp	x0, vector_table
	add	x0, x0, :lo12:vector_table
	msr	vbar_el1, x0

	adrp	x1, mmu_regs
	add	x1, x1, :lo12:mmu_regs
	ldp	x2, x3, [x1]
	ldr	x4, [x1, #16]
	msr	mair_el1, x2
	msr	tcr_el1, x3
	msr	ttbr0_el1, x4
	isb
	tlbi	vmalle1
	dsb	ish
	isb
	mrs	x0, sctlr_el1
	mov	x1, #((1 << 0) | (1 << 2) | (1 << 12))
	orr	x0, x0, x1
	msr	sctlr_el1, x0
	isb
	mov	x0, x20
	bl	secondary_main
	b	park

.section .bss
.balign 16
.global __stack_bottom
__stack_bottom:
	.skip	16384
.global __stack_top
__stack_top:

.balign 16
.global secondary_stacks
secondary_stacks:
	.skip	SECONDARY_STACK_SIZE * (MAX_CPUS + 1)

.section .data
.balign 8
.global boot_el
boot_el:
	.quad	0
.global boot_dtb
boot_dtb:
	.quad	0
