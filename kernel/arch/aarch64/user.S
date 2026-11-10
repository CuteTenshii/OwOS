/* user.S: EL0 test programs, copied to a user page and run at USER_BASE. Position independent. */

.section .rodata
.balign 8
.global user_hello_start, user_hello_end
user_hello_start:
	mov	x8, #1                       /* SYS_WRITE(fd, buf, len) */
	mov	x0, #1
	adr	x1, 1f
	mov	x2, #16
	svc	#0
	mov	x8, #2                       /* SYS_EXIT */
	mov	x0, #42
	svc	#0
0:	b	0b
1:	.ascii	"hello from EL0\n\0"
user_hello_end:

/* Exercises the syscall layer; exits with a bit per failed expectation (0 = all good). */
.global user_sys_start, user_sys_end
user_sys_start:
	mov	x19, #0
	mov	x8, #1                       /* write to an fd the kernel does not know: -1 */
	mov	x0, #5
	adr	x1, 1f
	mov	x2, #1
	svc	#0
	cmn	x0, #1
	cset	x1, ne
	orr	x19, x19, x1
	mov	x8, #999                     /* unknown syscall: -1 */
	svc	#0
	cmn	x0, #1
	cset	x1, ne
	orr	x19, x19, x1, lsl #1
	mov	x8, #1                       /* buffer outside the user window: -1 */
	mov	x0, #1
	movz	x1, #0x4000, lsl #16
	mov	x2, #4
	svc	#0
	cmn	x0, #1
	cset	x1, ne
	orr	x19, x19, x1, lsl #2
	mov	x8, #24                      /* SYS_MSEC before and after SYS_SLEEP(30) */
	svc	#0
	mov	x20, x0
	mov	x8, #22
	mov	x0, #30
	svc	#0
	mov	x8, #24
	svc	#0
	sub	x0, x0, x20
	cmp	x0, #30
	cset	x1, lo
	orr	x19, x19, x1, lsl #3
	mov	x8, #76                      /* SYS_GETPID: a positive thread id */
	svc	#0
	cmp	x0, #0
	cset	x1, le
	orr	x19, x19, x1, lsl #4
	mov	x8, #2
	mov	x0, x19
	svc	#0
0:	b	0b
1:	.ascii	"x"
user_sys_end:

.global user_fault_start, user_fault_end
user_fault_start:
	movz	x0, #0x4000, lsl #16         /* kernel RAM, not accessible from EL0 */
	str	x1, [x0]
0:	b	0b
user_fault_end:
