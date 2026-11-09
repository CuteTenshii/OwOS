/* start.S — user program entry. Placed first in the binary by user.ld, so
 * _start sits at the load address. Calls main(), then exit()s with its
 * return value. */

.section .text.start
.global _start
_start:
	call tls_init                  /* %gs segment for __thread data           */
	mov (%esp), %eax               /* argc (the kernel left it at the top)    */
	lea 4(%esp), %edx              /* argv = esp + 4                          */
	push %edx                      /* main(argc, argv)                        */
	push %eax
	call main

	mov %eax, %ebx                 /* exit code = main's return value         */
	mov $2, %eax                   /* SYS_EXIT                                */
	int $0x80
hang:
	jmp hang

/* setjmp/longjmp — the C89 non-local jump. jmp_buf is 6 words:
 *   [0]=ebx [1]=esi [2]=edi [3]=ebp [4]=esp(after ret) [5]=return address.
 * Only the callee-saved regs + the resume point are saved; the C caller preserves
 * everything else. Used for crash recovery (a signal handler longjmps out of a
 * faulting call chain back to a setjmp'd recovery point). */
.section .text
.global setjmp
setjmp:
	mov 4(%esp), %eax              /* jmp_buf *                                */
	mov %ebx, 0(%eax)
	mov %esi, 4(%eax)
	mov %edi, 8(%eax)
	mov %ebp, 12(%eax)
	lea 4(%esp), %ecx             /* caller's esp (past our return address)   */
	mov %ecx, 16(%eax)
	mov (%esp), %ecx              /* our return address = the resume point    */
	mov %ecx, 20(%eax)
	xor %eax, %eax                /* setjmp returns 0                         */
	ret

.global longjmp
longjmp:
	mov 4(%esp), %edx             /* jmp_buf *                                */
	mov 8(%esp), %eax             /* val                                      */
	test %eax, %eax
	jnz 1f
	inc %eax                      /* longjmp(env,0) makes setjmp return 1     */
1:
	mov 0(%edx), %ebx
	mov 4(%edx), %esi
	mov 8(%edx), %edi
	mov 12(%edx), %ebp
	mov 16(%edx), %esp            /* unwind the stack to the setjmp frame     */
	mov 20(%edx), %ecx
	jmp *%ecx                     /* resume as if setjmp returned `val`       */
