/* smp_ap_32.S: application-processor startup trampoline.
 *
 * The bytes between ap_trampoline_start/end are copied to a low page (SMP_TRAMP)
 * and an AP is released there by SIPI in 16-bit real mode. It switches to 32-bit
 * protected mode on a temporary flat GDT carried in this page, then jumps into
 * the kernel's ap_main on the stack the BSP staged in ap_stack_top.
 *
 * Two address kinds, and the distinction is load-bearing: references *inside* the
 * page must be SMP_TRAMP-relative (this code runs relocated, not at its link
 * address), while kernel symbols are reached by absolute address, hence ap_main
 * is called through a register, never a PC-relative `call`.
 */

#include "smp.h" /* SMP_TRAMP */

.section .text
.code16
.globl ap_trampoline_start
.globl ap_trampoline_end
ap_trampoline_start:
	cli
	cld
	xorw  %ax, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %ss
	lgdtl SMP_TRAMP + (ap_gdtr - ap_trampoline_start)
	movl  %cr0, %eax
	orl   $1, %eax
	movl  %eax, %cr0
	ljmpl $0x08, $(SMP_TRAMP + (ap_pm - ap_trampoline_start))

.code32
ap_pm:
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %ss
	movw  %ax, %fs
	movw  %ax, %gs
	movl  ap_cr3, %eax              /* enable paging on the kernel directory */
	movl  %eax, %cr3
	movl  %cr0, %eax
	orl   $0x80000000, %eax
	movl  %eax, %cr0
	movl  ap_stack_top, %esp        /* absolute: kernel global */
	pushl ap_arg
	movl  $ap_main, %eax
	call  *%eax                     /* absolute, not PC-relative */
1:	hlt
	jmp   1b

.align 16
ap_gdt:
	.quad 0x0000000000000000        /* null  */
	.quad 0x00CF9A000000FFFF        /* 0x08: flat 32-bit code */
	.quad 0x00CF92000000FFFF        /* 0x10: flat 32-bit data */
ap_gdt_end:
ap_gdtr:
	.word ap_gdt_end - ap_gdt - 1
	.long SMP_TRAMP + (ap_gdt - ap_trampoline_start)
ap_trampoline_end:
