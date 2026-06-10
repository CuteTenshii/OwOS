/* switch.S — kernel-thread context switch and new-thread trampoline. */

.section .text

/* void context_switch(uint32_t *old_esp, uint32_t new_esp);
 *
 * Save the callee-saved registers on the current stack, stash the resulting
 * esp into *old_esp, load new_esp, and restore that thread's registers. The
 * return address is already on each stack (from the original `call`), so the
 * final `ret` resumes the new thread wherever it last called context_switch.
 */
.global context_switch
.type context_switch, @function
context_switch:
	mov 4(%esp), %eax              /* old_esp (uint32_t *)                    */
	mov 8(%esp), %edx              /* new_esp (uint32_t)                      */

	push %ebp                      /* save callee-saved registers             */
	push %ebx
	push %esi
	push %edi

	mov %esp, (%eax)               /* *old_esp = current esp                  */
	mov %edx, %esp                 /* switch to the new stack                 */

	pop %edi                       /* restore the new thread's registers      */
	pop %esi
	pop %ebx
	pop %ebp
	ret                            /* resume the new thread                   */
.size context_switch, . - context_switch

/* A freshly created thread's crafted stack returns here on first run. We
 * enter with interrupts disabled (deep inside the timer IRQ that switched to
 * us), so enable them before the thread body runs; then `ret` jumps to the
 * entry function whose address sits just above on the stack. */
.global thread_start
.type thread_start, @function
thread_start:
	sti
	ret
.size thread_start, . - thread_start

/* A freshly created user task's crafted kernel stack returns here on its
 * first schedule. The iret frame (user ss/esp/eflags/cs/eip) sits just above,
 * so we load the user data segments and iret down to ring 3. */
.global enter_user_task
.type enter_user_task, @function
enter_user_task:
	mov $0x23, %ax                 /* user data selector                       */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	iret
.size enter_user_task, . - enter_user_task

/* A forked child's first schedule lands here: its kernel stack holds a full
 * copy of the parent's syscall trap frame (with eax forced to 0). This is the
 * tail of isr_common_stub -- restore segments + registers and iret to ring 3,
 * so the child returns from fork() with 0. */
.global fork_child_entry
.type fork_child_entry, @function
fork_child_entry:
	pop %eax                       /* saved data segment                       */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	popa
	add $8, %esp                   /* drop int_no + err_code                   */
	iret
.size fork_child_entry, . - fork_child_entry
