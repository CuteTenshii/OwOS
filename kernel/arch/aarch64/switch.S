/* switch.S: kernel context switch (AAPCS64 callee-saved registers) and the first code a new task runs; task.c builds
 * the context each one starts from. */

.section .text

/* void context_switch(uintptr_t *old_sp, uintptr_t new_sp);  (x0, x1)
 * A context is x19..x30 on its stack, 96 bytes: switching saves the current one, then resumes `new_sp`. */
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

/* A new kernel thread: it holds the kernel lock like any running kernel code, runs entry (x19) with interrupts on,
 * and ends when entry returns. */
.global thread_start
.type thread_start, %function
thread_start:
	bl	sched_kernel_enter
	msr	daifclr, #2
	blr	x19
	bl	thread_exit
0:	b	0b
.size thread_start, . - thread_start

/* A new user task: drop to EL0t at x19 on user stack x20, from the empty kernel stack x21 that its traps will then
 * land on, every register zero. */
.global enter_user_task
.type enter_user_task, %function
enter_user_task:
	msr	daifset, #0xf /* an interrupt from here to the eret would overwrite ELR and SPSR */
	mov	sp, x21
	msr	sp_el0, x20
	msr	elr_el1, x19
	msr	spsr_el1, xzr
	mov	x0, #0
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x5, #0
	mov	x6, #0
	mov	x7, #0
	mov	x8, #0
	mov	x9, #0
	mov	x10, #0
	mov	x11, #0
	mov	x12, #0
	mov	x13, #0
	mov	x14, #0
	mov	x15, #0
	mov	x16, #0
	mov	x17, #0
	mov	x18, #0
	mov	x19, #0
	mov	x20, #0
	mov	x21, #0
	mov	x22, #0
	mov	x23, #0
	mov	x24, #0
	mov	x25, #0
	mov	x26, #0
	mov	x27, #0
	mov	x28, #0
	mov	x29, #0
	mov	x30, #0
	eret
.size enter_user_task, . - enter_user_task

/* A forked child: x19 is its copy of the parent's syscall trap frame, x0 already zero. */
.global fork_child_entry
.type fork_child_entry, %function
fork_child_entry:
	mov	sp, x19
	b	trap_restore
.size fork_child_entry, . - fork_child_entry
