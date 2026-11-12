/* irq_common_64.S: the 64-bit landing pad every irq_stubs.S stub jumps to. */

.section .text

.extern irq_handler
/* 64-bit IRQ landing pad: mirrors isr.S's 64-bit common stub but calls
 * irq_handler. See idt.h for the struct registers layout the push order builds. */
.global irq_common_stub
.type irq_common_stub, @function
irq_common_stub:
	push %rax
	push %rcx
	push %rdx
	push %rbx
	lea  32(%rsp), %rax
	push %rax
	push %rbp
	push %rsi
	push %rdi
	push %r8
	push %r9
	push %r10
	push %r11
	push %r12
	push %r13
	push %r14
	push %r15
	xor  %rax, %rax
	mov  %ds, %ax
	push %rax

	cld                            /* user DF=1 would run the kernel's rep movs/stos backwards */
	mov  %rsp, %rdi                /* arg: pointer to struct registers        */
	call irq_handler

	/* DS is per CPU, not per task: a task resumed on a CPU that ran the kernel would iretq to ring 3 with
	 * DPL-0 DS, which the CPU nulls, and compatibility mode then faults on every DS access. */
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
.size irq_common_stub, . - irq_common_stub
