/* smp_ap_64.S: application-processor startup trampoline.
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

/* x86_64 AP bring-up: real mode -> 32-bit protected -> long mode. Same handoff
 * as the 32-bit path (ap_cr3 holds the kernel PML4, ap_stack_top/ap_arg are
 * staged by the BSP, ap_main is reached by absolute address), with an extra
 * PAE+LME+paging step and a 64-bit code segment in the trampoline GDT (0x18).
 * Inside-the-page references are SMP_TRAMP-relative; kernel symbols are absolute
 * (movabs the link address, then dereference; it is identity-mapped). */
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
	movl  %cr4, %eax
	orl   $0x20, %eax              /* CR4.PAE                                 */
	movl  %eax, %cr4
	movl  ap_cr3, %eax             /* absolute: kernel PML4 phys (< 4 GiB)    */
	movl  %eax, %cr3
	movl  $0xC0000080, %ecx        /* EFER                                    */
	rdmsr
	orl   $0x100, %eax             /* EFER.LME                                */
	wrmsr
	movl  %cr0, %eax
	orl   $0x80000000, %eax        /* CR0.PG                                  */
	movl  %eax, %cr0
	ljmpl $0x18, $(SMP_TRAMP + (ap_long - ap_trampoline_start))

.code64
ap_long:
	movw  $0x10, %ax
	movw  %ax, %ds
	movw  %ax, %es
	movw  %ax, %ss
	movw  %ax, %fs
	movw  %ax, %gs
	movabs $ap_stack_top, %rax     /* kernel globals: absolute link address   */
	movq  (%rax), %rsp
	movabs $ap_arg, %rax
	movq  (%rax), %rdi             /* arg1 to ap_main                         */
	movabs $ap_main, %rax
	call  *%rax
1:	hlt
	jmp   1b

.align 16
ap_gdt:
	.quad 0x0000000000000000        /* null  */
	.quad 0x00CF9A000000FFFF        /* 0x08: flat 32-bit code */
	.quad 0x00CF92000000FFFF        /* 0x10: flat data */
	.quad 0x00AF9A000000FFFF        /* 0x18: 64-bit code (L=1) */
ap_gdt_end:
ap_gdtr:
	.word ap_gdt_end - ap_gdt - 1
	.long SMP_TRAMP + (ap_gdt - ap_trampoline_start)
ap_trampoline_end:
