/* vectors.S: EL1 exception vector table and register save/restore. */

.macro ventry kind
	.balign 128
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\kind
	b	trap_common
.endm

.section .text
.balign 2048
.global vector_table
vector_table:
	ventry 0
	ventry 1
	ventry 2
	ventry 3
	ventry 4
	ventry 5
	ventry 6
	ventry 7
	ventry 8
	ventry 9
	ventry 10
	ventry 11
	ventry 12
	ventry 13
	ventry 14
	ventry 15

/* Frame is struct trap_frame (272 bytes); x29/x30 were already pushed by ventry. */
trap_common:
	sub	sp, sp, #256
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	ldp	x0, x1, [sp, #256]           /* original x29, x30 */
	stp	x0, x1, [sp, #232]
	mrs	x0, sp_el0
	mrs	x1, elr_el1
	mrs	x2, spsr_el1
	stp	x0, x1, [sp, #248]
	str	x2, [sp, #264]

	/* x29 still holds the vector kind; tf lives at sp, which now spans 272 bytes. */
	mov	x1, x29
	mov	x0, sp
	bl	arch_exception

	ldr	x2, [sp, #264]
	ldp	x0, x1, [sp, #248]
	msr	sp_el0, x0
	msr	elr_el1, x1
	msr	spsr_el1, x2
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	ldp	x29, x30, [sp, #232]
	add	sp, sp, #272
	eret
