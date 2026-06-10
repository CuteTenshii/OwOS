/* gdt_flush.S — load the GDT and make the CPU actually use it.
 *
 * lgdt only points the CPU at the table; the segment registers still hold
 * the old selectors until reloaded. Data segments reload with a plain mov,
 * but CS can only change via a far jump (or far return) — that's what makes
 * the new code selector take effect.
 *
 * void gdt_flush(uint32_t gdt_ptr_addr);
 */

.section .text
.global gdt_flush
.type gdt_flush, @function
gdt_flush:
	mov 4(%esp), %eax              /* arg0: address of the gdt_ptr            */
	lgdt (%eax)

	mov $0x10, %ax                 /* 0x10 = kernel data selector (GDT_KERNEL_DATA) */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss

	ljmp $0x08, $.reload_cs        /* 0x08 = kernel code selector; far jump reloads CS */
.reload_cs:
	ret
.size gdt_flush, . - gdt_flush
