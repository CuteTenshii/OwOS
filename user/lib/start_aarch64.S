/* start_aarch64.S: AArch64 user program entry. Placed first in the binary by user.ld, so _start sits at the load
 * address. The kernel leaves argc at a 16-byte aligned sp with the argv pointers above it; main's return value goes
 * to exit(). System calls are svc #0 with the number in x8 and arguments from x0. */

.section .text.start
.global _start
_start:
	mov	x29, #0                      /* outermost frame for backtraces */
	mov	x30, #0
	mov	x19, sp                      /* callee-saved, so it survives tls_init */
	bl	tls_init                     /* TPIDR_EL0 for __thread data */
	ldr	w0, [x19]                    /* main(argc, argv) */
	add	x1, x19, #8
	bl	main
	mov	x8, #2                       /* SYS_EXIT(main's return value) */
	svc	#0
hang:
	b	hang

/* A new thread starts here (see thread_spawn in lib.c) with sp at {fn, arg} and 16-byte aligned past them. A fn
 * that returns ends only its own thread, as exit does from a thread. */
.section .text
.global thread_entry
.type thread_entry, %function
thread_entry:
	mov	x29, #0
	mov	x30, #0
	ldp	x9, x0, [sp], #16            /* fn, arg */
	blr	x9
	mov	x0, #0
	mov	x8, #2                       /* SYS_EXIT */
	svc	#0
	b	hang
.size thread_entry, . - thread_entry

/* setjmp/longjmp: the C89 non-local jump. jmp_buf is 21 words: [0..11] = x19..x30, [12] = sp, [13..20] = d8..d15,
 * the AAPCS64 callee-saved state; x30 is the resume point. */
.global setjmp
.type setjmp, %function
setjmp:
	stp	x19, x20, [x0, #0]
	stp	x21, x22, [x0, #16]
	stp	x23, x24, [x0, #32]
	stp	x25, x26, [x0, #48]
	stp	x27, x28, [x0, #64]
	stp	x29, x30, [x0, #80]
	mov	x2, sp
	str	x2, [x0, #96]
	stp	d8, d9, [x0, #104]
	stp	d10, d11, [x0, #120]
	stp	d12, d13, [x0, #136]
	stp	d14, d15, [x0, #152]
	mov	w0, #0                       /* setjmp returns 0 */
	ret
.size setjmp, . - setjmp

.global longjmp
.type longjmp, %function
longjmp:
	ldp	x19, x20, [x0, #0]
	ldp	x21, x22, [x0, #16]
	ldp	x23, x24, [x0, #32]
	ldp	x25, x26, [x0, #48]
	ldp	x27, x28, [x0, #64]
	ldp	x29, x30, [x0, #80]
	ldr	x2, [x0, #96]
	mov	sp, x2                       /* unwind the stack to the setjmp frame */
	ldp	d8, d9, [x0, #104]
	ldp	d10, d11, [x0, #120]
	ldp	d12, d13, [x0, #136]
	ldp	d14, d15, [x0, #152]
	cmp	w1, #0
	csinc	w0, w1, wzr, ne              /* longjmp(env, 0) makes setjmp return 1 */
	ret                                  /* to x30: as if setjmp returned `val` */
.size longjmp, . - longjmp
