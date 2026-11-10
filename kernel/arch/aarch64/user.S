/* user.S: EL0 test programs, copied to a user page and run at USER_BASE. Position independent. */

.section .rodata
.balign 8
.global user_hello_start, user_hello_end
user_hello_start:
	mov	x8, #1                       /* SYS_WRITE */
	adr	x0, 1f
	mov	x1, #16
	svc	#0
	mov	x8, #2                       /* SYS_EXIT */
	mov	x0, #42
	svc	#0
0:	b	0b
1:	.ascii	"hello from EL0\n\0"
user_hello_end:

.global user_fault_start, user_fault_end
user_fault_start:
	movz	x0, #0x4000, lsl #16         /* kernel RAM, not accessible from EL0 */
	str	x1, [x0]
0:	b	0b
user_fault_end:
