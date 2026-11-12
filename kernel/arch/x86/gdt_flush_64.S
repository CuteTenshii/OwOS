/* gdt_flush_64.S: load the GDT and make the CPU actually use it.
 *
 * lgdt only points the CPU at the table; the segment registers still hold
 * the old selectors until reloaded. Data segments reload with a plain mov,
 * but CS can only change via a far jump (or far return), and that is what makes
 * the new code selector take effect.
 */

.section .text
.global gdt_flush
.type gdt_flush, @function
/* void gdt_flush(uint64_t gdt_ptr_addr); arg in %rdi. CS can't be reloaded by a
 * far jump in long mode (no direct far jmp), so reload it with a far return. */
gdt_flush:
	lgdt (%rdi)

	mov $0x10, %ax                 /* kernel data selector                    */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss

	leaq .reload_cs(%rip), %rax
	pushq $0x08                    /* kernel code selector                    */
	pushq %rax
	lretq                          /* far return reloads CS                   */
.reload_cs:
	ret
.size gdt_flush, . - gdt_flush
