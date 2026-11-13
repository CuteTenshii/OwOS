/* switch_64.S: kernel-thread context switch and new-thread trampoline. */

.section .text

/* void context_switch(uintptr_t *old_sp, uintptr_t new_sp);  (rdi, rsi)
 *
 * Save the SysV callee-saved registers (rbx, rbp, r12-r15) on the current stack,
 * stash rsp into *old_sp, load new_sp, restore that thread's registers, and ret
 * into wherever it last called context_switch. */
.global context_switch
.type context_switch, @function
context_switch:
	push %rbp
	push %rbx
	push %r12
	push %r13
	push %r14
	push %r15

	mov %rsp, (%rdi)               /* *old_sp = current rsp                   */
	mov %rsi, %rsp                 /* switch to the new stack                 */

	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %rbx
	pop %rbp
	ret
.size context_switch, . - context_switch

/* A freshly created thread's crafted stack returns here on first run; enable
 * interrupts (we arrive with IF=0 from the switching IRQ) then ret to the entry
 * function whose address sits just above on the stack. */
.global thread_start
.type thread_start, @function
thread_start:
	call sched_kernel_enter
	sti
	ret
.size thread_start, . - thread_start

/* A freshly created user task's crafted kernel stack holds an iretq frame
 * (ss/rsp/rflags/cs/rip). Drop to ring 3. Segments are flat in long mode; the
 * iretq frame carries SS, and CS selects the user code segment. %fs is left
 * alone: loading it would reset the TLS base the scheduler just set, and the
 * GPRs are cleared so no kernel value reaches user code. */
.global enter_user_task
.type enter_user_task, @function
enter_user_task:
	mov $0x23, %ax                 /* user data selector                       */
	mov %ax, %ds
	mov %ax, %es
	xor %eax, %eax
	xor %ebx, %ebx
	xor %ecx, %ecx
	xor %edx, %edx
	xor %esi, %esi
	xor %edi, %edi
	xor %ebp, %ebp
	xor %r8d, %r8d
	xor %r9d, %r9d
	xor %r10d, %r10d
	xor %r11d, %r11d
	xor %r12d, %r12d
	xor %r13d, %r13d
	xor %r14d, %r14d
	xor %r15d, %r15d
	iretq
.size enter_user_task, . - enter_user_task

/* A forked child's first schedule lands here: its kernel stack holds a full copy
 * of the parent's syscall trap frame (struct registers, with eax forced to 0).
 * This is the tail of isr_common_stub for 64-bit: restore the GPRs and iretq to
 * ring 3, so the child returns from fork() with 0. */
.global fork_child_entry
.type fork_child_entry, @function
fork_child_entry:
	/* DS is per CPU, not per task: a task resumed on a CPU that ran the kernel would iretq to ring 3 with
	 * DPL-0 DS, which the CPU nulls. */
	pop  %rax                      /* ds saved at entry                        */
	mov  %ax, %ds
	mov  %ax, %es
	pop  %r15
	pop  %r14
	pop  %r13
	pop  %r12
	pop  %r11
	pop  %r10
	pop  %r9
	pop  %r8
	pop  %rdi
	pop  %rsi
	pop  %rbp
	add  $8, %rsp                  /* drop the esp slot                       */
	pop  %rbx
	pop  %rdx
	pop  %rcx
	pop  %rax
	add  $16, %rsp                 /* drop int_no and err_code                */
	iretq
.size fork_child_entry, . - fork_child_entry
