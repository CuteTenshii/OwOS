/* boot.S: AArch64 entry: park secondary cores, drop to EL1, clear BSS, enter C. */

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

	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	adrp	x1, boot_el
	str	x0, [x1, :lo12:boot_el]
	cmp	x0, #2
	b.ne	at_el1

	mov	x0, #(1 << 31)               /* HCR_EL2.RW: EL1 is AArch64 */
	msr	hcr_el2, x0
	mov	x0, #0x33ff                  /* CPTR_EL2: no FP/SIMD traps */
	msr	cptr_el2, x0
	mrs	x0, cnthctl_el2
	orr	x0, x0, #3                   /* EL1 may use the physical counter and timer */
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr
	mov	x0, #0x3c5                   /* SPSR: EL1h, DAIF masked */
	msr	spsr_el2, x0
	adr	x0, at_el1
	msr	elr_el2, x0
	eret

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

.section .bss
.balign 16
.global __stack_bottom
__stack_bottom:
	.skip	16384
.global __stack_top
__stack_top:

.section .data
.balign 8
.global boot_el
boot_el:
	.quad	0
.global boot_dtb
boot_dtb:
	.quad	0
