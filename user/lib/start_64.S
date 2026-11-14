/* start_64.S: x86_64 user program entry. Placed first in the binary by user.ld, so
 * _start sits at the load address. The kernel leaves argc at a 16-byte aligned
 * %rsp with the argv pointers above it; main's return value goes to exit(). */

.section .text.start
.global _start
_start:
	xor  %ebp, %ebp                /* outermost frame for backtraces           */
	mov  %rsp, %rbx                /* callee-saved, so it survives tls_init    */
	and  $-16, %rsp
	call tls_init                  /* %fs base for __thread data               */
	mov  (%rbx), %edi              /* main(argc, argv)                         */
	lea  8(%rbx), %rsi
	call main

	mov  %eax, %ebx                /* exit code = main's return value          */
	mov  $2, %eax                  /* SYS_EXIT                                 */
	int  $0x80
hang:
	jmp  hang

/* A new thread starts here (see thread_spawn in lib.c) with %rsp at {fn, arg} and 16-byte aligned past them. A fn
 * that returns ends only its own thread, as exit does from a thread. */
.section .text
.global thread_entry
.type thread_entry, @function
thread_entry:
	xor  %ebp, %ebp
	pop  %rax                      /* fn                                       */
	pop  %rdi                      /* arg                                      */
	call *%rax
	xor  %ebx, %ebx
	mov  $2, %eax                  /* SYS_EXIT                                 */
	int  $0x80
	jmp  hang
.size thread_entry, . - thread_entry

/* setjmp/longjmp: the C89 non-local jump. jmp_buf is 8 quads:
 *   [0]=rbx [1]=rbp [2..5]=r12..r15 [6]=rsp(after ret) [7]=return address.
 * Only the SysV callee-saved registers and the resume point are kept. */
.section .text
.global setjmp
.type setjmp, @function
setjmp:
	mov  %rbx, 0(%rdi)
	mov  %rbp, 8(%rdi)
	mov  %r12, 16(%rdi)
	mov  %r13, 24(%rdi)
	mov  %r14, 32(%rdi)
	mov  %r15, 40(%rdi)
	lea  8(%rsp), %rdx             /* caller's rsp (past our return address)   */
	mov  %rdx, 48(%rdi)
	mov  (%rsp), %rdx              /* our return address = the resume point    */
	mov  %rdx, 56(%rdi)
	xor  %eax, %eax                /* setjmp returns 0                         */
	ret
.size setjmp, . - setjmp

.global longjmp
.type longjmp, @function
longjmp:
	mov  %esi, %eax
	test %eax, %eax
	jnz  1f
	inc  %eax                      /* longjmp(env,0) makes setjmp return 1     */
1:
	mov  0(%rdi), %rbx
	mov  8(%rdi), %rbp
	mov  16(%rdi), %r12
	mov  24(%rdi), %r13
	mov  32(%rdi), %r14
	mov  40(%rdi), %r15
	mov  48(%rdi), %rsp            /* unwind the stack to the setjmp frame     */
	jmp  *56(%rdi)                 /* resume as if setjmp returned `val`       */
.size longjmp, . - longjmp
