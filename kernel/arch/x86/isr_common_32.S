/* isr_common_32.S: the 32-bit landing pad every isr.S stub jumps to. */

.section .text

/* Common landing pad. On entry the stack already holds (low -> high):
 *   err_code, int_no, eip, cs, eflags  (no ss/esp: we stay in ring 0).
 * We add the saved general registers and data segment to match
 * struct registers in idt.c, then hand a pointer to that frame to C. */
.extern isr_handler
.global isr_common_stub
.type isr_common_stub, @function
isr_common_stub:
	pusha                          /* edi,esi,ebp,esp,ebx,edx,ecx,eax        */

	mov %ds, %ax                   /* save the current data segment           */
	push %eax

	mov $0x10, %ax                 /* switch to the kernel data segment       */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs                   /* %gs stays: it is the user's TLS segment */

	cld
	push %esp                      /* arg: pointer to struct registers        */
	call isr_handler
	add $4, %esp

	pop %eax                       /* restore the original data segment       */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs

	popa
	add $8, %esp                   /* drop int_no and err_code                */
	iret                           /* pops eip, cs, eflags (restores IF)      */
.size isr_common_stub, . - isr_common_stub
