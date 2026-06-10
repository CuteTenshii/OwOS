/* start.S — user program entry. Placed first in the binary by user.ld, so
 * _start sits at the load address. Calls main(), then exit()s with its
 * return value. */

.section .text.start
.global _start
_start:
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
