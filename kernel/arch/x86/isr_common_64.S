/* isr_common_64.S: the 64-bit landing pad every isr.S stub jumps to. */

.section .text

.extern isr_handler
/* No pusha in long mode: push each GPR. The CPU always pushes SS:RSP, so the frame is uniform. Segments are
 * flat, so entry does not reload them; %ds is recorded so the exit path can restore it. Push order builds
 * struct registers (idt.h) bottom-up: ds is lowest, then r15..r8, edi/esi/ebp/esp/ebx/edx/ecx/eax. */
.global isr_common_stub
.type isr_common_stub, @function
isr_common_stub:                       /* on entry: top of stack = int_no       */
	push %rax
	push %rcx
	push %rdx
	push %rbx
	lea  32(%rsp), %rax            /* trap rsp (4 quads pushed so far) -> esp   */
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
	push %rax                      /* ds slot                                  */

	cld                            /* user DF=1 would run the kernel's rep movs/stos backwards */
	mov  %rsp, %rdi                /* arg: pointer to struct registers         */
	call isr_handler

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
	add  $8, %rsp                  /* drop the esp slot (not a real register)  */
	pop  %rbx
	pop  %rdx
	pop  %rcx
	pop  %rax
	add  $16, %rsp                 /* drop int_no and err_code                 */
	iretq
.size isr_common_stub, . - isr_common_stub
