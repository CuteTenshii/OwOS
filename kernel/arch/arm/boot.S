#include "cpu_defs.h"

/* boot.S: ARMv7-A entry: park secondary cores, leave HYP for SVC, clear BSS, enter C.
 * The loader follows the Linux boot protocol: MMU and D-cache off, r2 = device tree. */

	.syntax unified
	.arm
	.arch_extension virt

.section .text.boot
.global _start
_start:
	mov	r8, r2
	mrc	p15, 0, r0, c0, c0, 5        /* MPIDR */
	ands	r0, r0, #0xff
	bne	park

	mrs	r0, cpsr
	and	r9, r0, #PSR_MODE_MASK
	cmp	r9, #PSR_MODE_HYP
	bne	in_svc

	/* SVC code may then use the counters and timers, sees the real IDs and is never trapped to HYP. */
	mrc	p15, 4, r0, c14, c1, 0       /* CNTHCTL */
	orr	r0, r0, #3
	mcr	p15, 4, r0, c14, c1, 0
	mov	r0, #0
	mov	r1, #0
	mcrr	p15, 4, r0, r1, c14          /* CNTVOFF */
	mcr	p15, 4, r0, c1, c1, 0        /* HCR */
	mcr	p15, 4, r0, c1, c1, 3        /* HSTR */
	mrc	p15, 0, r0, c0, c0, 0
	mcr	p15, 4, r0, c0, c0, 0        /* VPIDR = MIDR */
	mrc	p15, 0, r0, c0, c0, 5
	mcr	p15, 4, r0, c0, c0, 5        /* VMPIDR = MPIDR */
	mov	r0, #(PSR_MODE_SVC | PSR_A | PSR_I | PSR_F)
	msr	spsr_hyp, r0
	adr	r0, in_svc
	msr	elr_hyp, r0
	eret

in_svc:
	cpsid	aif, #PSR_MODE_SVC
	ldr	sp, =__stack_top

	ldr	r0, =vector_table
	mcr	p15, 0, r0, c12, c0, 0       /* VBAR */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 13)           /* SCTLR.V: take exceptions through VBAR, not 0xffff0000 */
	mcr	p15, 0, r0, c1, c0, 0
	isb

	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	mov	r2, #0
	mov	r3, #0
1:	cmp	r0, r1
	strdlo	r2, r3, [r0], #8
	blo	1b

	ldr	r0, =boot_dtb
	str	r8, [r0]
	ldr	r0, =boot_mode
	str	r9, [r0]
	bl	arch_main

park:	wfe
	b	park

	.ltorg

.section .bss
.balign 16
.global __stack_bottom
__stack_bottom:
	.skip	BOOT_STACK_SIZE
.global __stack_top
__stack_top:

.section .data
.balign 4
.global boot_dtb
boot_dtb:
	.word	0
.global boot_mode
boot_mode:
	.word	0
