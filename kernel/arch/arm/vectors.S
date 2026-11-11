#include "cpu_defs.h"

/* vectors.S: exception vector table. Every exception is handled on the SVC stack of the code it interrupted, so an
 * IRQ handler can switch threads like any other kernel code; the banked IRQ/abort/undef stacks are never used. */

	.syntax unified
	.arm

/* lr_adjust turns the banked lr into the address to resume at (IRQ) or of the faulting instruction (undef, aborts). */
.macro trap name, kind, lr_adjust
\name:
	.if \lr_adjust
	sub	lr, lr, #\lr_adjust
	.endif
	srsdb	sp!, #PSR_MODE_SVC
	cps	#PSR_MODE_SVC
	push	{r0-r12, lr}
	mov	r1, #\kind
	b	trap_common
.endm

.section .text
.balign 32
.global vector_table
vector_table:
	b	trap_reset
	b	trap_undef
	b	trap_svc
	b	trap_pabt
	b	trap_dabt
	b	trap_unused
	b	trap_irq
	b	trap_fiq

	trap	trap_reset, TRAP_RESET, 0
	trap	trap_undef, TRAP_UNDEF, 4
	trap	trap_svc, TRAP_SVC, 0
	trap	trap_pabt, TRAP_PABT, 4
	trap	trap_dabt, TRAP_DABT, 8
	trap	trap_unused, TRAP_UNUSED, 0
	trap	trap_irq, TRAP_IRQ, 4
	trap	trap_fiq, TRAP_FIQ, 4

/* sp points at the struct trap_frame just built. The interrupted code may have left sp only 4-byte aligned, and the
 * AAPCS wants 8 at a call: r4 keeps the padding, it is callee-saved and already in the frame. */
trap_common:
	mov	r0, sp
	and	r4, sp, #4
	sub	sp, sp, r4
	bl	arch_exception
	add	sp, sp, r4
	pop	{r0-r12, lr}
	rfeia	sp!
