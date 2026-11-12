/* irq_common_32.S: the 32-bit landing pad every irq_stubs.S stub jumps to. */

.section .text

.extern irq_handler
.global irq_common_stub
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
