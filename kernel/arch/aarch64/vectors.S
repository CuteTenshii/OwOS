/* vectors.S: EL1 exception vector table and the trap frame (struct registers, regs.h) around arch_exception. */

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

/* struct registers is 320 bytes: x0..x30 at 0..240, sp 248, pc 256, pstate 264, sa[3] 272..288, esr 296, far 304,
 * pad 312. ventry already pushed x29/x30 where far and pad go, so they are moved to their slots first. */
trap_common:
	sub	sp, sp, #304
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
	ldp	x0, x1, [sp, #304]           /* original x29, x30 */
	stp	x0, x1, [sp, #232]
	mrs	x0, sp_el0
	mrs	x1, elr_el1
	stp	x0, x1, [sp, #248]
	mrs	x0, spsr_el1
	str	x0, [sp, #264]
	mrs	x0, esr_el1
	mrs	x1, far_el1
	str	x0, [sp, #296]
	stp	x1, xzr, [sp, #304]

	/* x29 still holds the vector kind. */
	mov	x1, x29
	mov	x0, sp
	bl	arch_exception

/* Return to the context in the frame at sp; a forked child enters here on its copy of the parent's frame. */
.global trap_restore
trap_restore:
	/* The handler may return with interrupts on; one taken from here on would overwrite ELR and SPSR. */
	msr	daifset, #0xf
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
	add	sp, sp, #320
	eret
