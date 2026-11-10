/* switch.S: kernel-thread context switch (AAPCS64 callee-saved registers). */

.section .text

/* void context_switch(uintptr_t *old_sp, uintptr_t new_sp);  (x0, x1) */
.global context_switch
.type context_switch, %function
context_switch:
	sub	sp, sp, #96
	stp	x19, x20, [sp, #0]
	stp	x21, x22, [sp, #16]
	stp	x23, x24, [sp, #32]
	stp	x25, x26, [sp, #48]
	stp	x27, x28, [sp, #64]
	stp	x29, x30, [sp, #80]
	mov	x2, sp
	str	x2, [x0]
	mov	sp, x1
	ldp	x19, x20, [sp, #0]
	ldp	x21, x22, [sp, #16]
	ldp	x23, x24, [sp, #32]
	ldp	x25, x26, [sp, #48]
	ldp	x27, x28, [sp, #64]
	ldp	x29, x30, [sp, #80]
	add	sp, sp, #96
	ret
.size context_switch, . - context_switch

/* First code a new thread runs: context_init leaves its entry in x19. */
.global thread_trampoline
.type thread_trampoline, %function
thread_trampoline:
	msr	daifclr, #2
	blr	x19
	mov	x0, #0
	bl	thread_exit
.size thread_trampoline, . - thread_trampoline
