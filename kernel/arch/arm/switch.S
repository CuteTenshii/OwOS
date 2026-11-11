/* switch.S: kernel-thread context switch (AAPCS callee-saved registers). */

	.syntax unified
	.arm

.section .text

/* void context_switch(uintptr_t *old_sp, uintptr_t new_sp);  (r0, r1)
 * r12 is not callee-saved; it only rounds the frame to ten words so sp stays 8-byte aligned. */
.global context_switch
.type context_switch, %function
context_switch:
	push	{r4-r12, lr}
	str	sp, [r0]
	mov	sp, r1
	pop	{r4-r12, lr}
	bx	lr
.size context_switch, . - context_switch

/* First code a new thread runs: context_init leaves entry in r4 and its argument in r5. It is entered from a switch
 * made with interrupts masked. */
.global thread_trampoline
.type thread_trampoline, %function
thread_trampoline:
	cpsie	i
	mov	r0, r5
	blx	r4
	mov	r0, #0
	bl	thread_exit
.size thread_trampoline, . - thread_trampoline
