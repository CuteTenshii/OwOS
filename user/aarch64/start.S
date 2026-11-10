/* start.S: user program entry; the kernel hands over a 16-byte aligned sp. */
.section .text.start
.global _start
_start:
	bl	main
	mov	x8, #2                       /* SYS_EXIT(main's return value) */
	svc	#0
0:	b	0b
