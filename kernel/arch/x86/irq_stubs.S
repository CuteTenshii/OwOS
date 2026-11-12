/* irq.S: stubs for the 16 hardware IRQs, remapped to vectors 32-47.
 *
 * IRQs never push an error code, so every stub pushes a dummy zero (to match
 * struct registers) followed by its vector number, then falls into a common
 * stub that mirrors isr.S but dispatches to irq_handler.
 */

.section .text

.macro IRQ num, vec
.global irq\num
.type irq\num, @function
irq\num:
	push $0                        /* dummy error code                        */
	push $\vec                     /* vector number (32 + IRQ)                */
	jmp irq_common_stub
.endm

IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* Local APIC timer (the per-core scheduler tick on APs), vector 0x40. Same
 * frame shape as an IRQ; irq_handler special-cases it (LAPIC EOI, not PIC). */
.global lapic_timer_isr
.type lapic_timer_isr, @function
lapic_timer_isr:
	push $0
	push $0x40
	jmp irq_common_stub

.extern irq_handler
#ifdef __x86_64__
/* 64-bit IRQ landing pad: mirrors isr.S's 64-bit common stub but calls
 * irq_handler. See idt.h for the struct registers layout the push order builds. */
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
#else
.type irq_common_stub, @function
irq_common_stub:
	pusha

	mov %ds, %ax
	push %eax

	mov $0x10, %ax                 /* kernel data segment                     */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs

	cld
	push %esp                      /* arg: pointer to struct registers        */
	call irq_handler
	add $4, %esp

	pop %eax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs

	popa
	add $8, %esp                   /* drop int_no and err_code                */
	iret
.size irq_common_stub, . - irq_common_stub
#endif
