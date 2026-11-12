/* isr.S — interrupt service routine stubs for the 32 CPU exceptions.
 *
 * Each vector gets a tiny stub. Some exceptions make the CPU push a 4-byte
 * error code, most do not; to give the C handler one uniform stack frame,
 * the no-error-code stubs push a dummy zero. Every stub then pushes its
 * vector number and falls into a common stub that saves the full register
 * state, calls isr_handler(struct registers *), and returns with iret.
 */

.section .text

/* Stub for vectors that DON'T push an error code: push a dummy 0 first. */
.macro ISR_NOERR num
.global isr\num
.type isr\num, @function
isr\num:
	push $0
	push $\num
	jmp isr_common_stub
.endm

/* Stub for vectors where the CPU already pushed a real error code. */
.macro ISR_ERR num
.global isr\num
.type isr\num, @function
isr\num:
	push $\num
	jmp isr_common_stub
.endm

ISR_NOERR 0     /* #DE  divide error                  */
ISR_NOERR 1     /* #DB  debug                         */
ISR_NOERR 2     /*      non-maskable interrupt         */
ISR_NOERR 3     /* #BP  breakpoint                    */
ISR_NOERR 4     /* #OF  overflow                      */
ISR_NOERR 5     /* #BR  bound range exceeded          */
ISR_NOERR 6     /* #UD  invalid opcode                */
ISR_NOERR 7     /* #NM  device not available          */
ISR_ERR   8     /* #DF  double fault                  */
ISR_NOERR 9     /*      coprocessor segment overrun   */
ISR_ERR   10    /* #TS  invalid TSS                   */
ISR_ERR   11    /* #NP  segment not present           */
ISR_ERR   12    /* #SS  stack-segment fault           */
ISR_ERR   13    /* #GP  general protection            */
ISR_ERR   14    /* #PF  page fault                    */
ISR_NOERR 15    /*      reserved                      */
ISR_NOERR 16    /* #MF  x87 floating-point            */
ISR_ERR   17    /* #AC  alignment check               */
ISR_NOERR 18    /* #MC  machine check                 */
ISR_NOERR 19    /* #XM  SIMD floating-point           */
ISR_NOERR 20    /* #VE  virtualization                */
ISR_ERR   21    /* #CP  control protection            */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29    /* #VC  VMM communication             */
ISR_ERR   30    /* #SX  security                      */
ISR_NOERR 31    /*      reserved                      */

ISR_NOERR 128   /* int 0x80: system call (gate is DPL 3) */

/* Common landing pad. On entry the stack already holds (low -> high):
 *   err_code, int_no, eip, cs, eflags  (no ss/esp: we stay in ring 0).
 * We add the saved general registers and data segment to match
 * struct registers in idt.c, then hand a pointer to that frame to C. */
.extern isr_handler
#ifdef __x86_64__
/* 64-bit: no pusha; push each GPR. The CPU always pushes SS:RSP, so the frame is
 * uniform. Segment registers are flat/ignored in long mode, so we don't reload
 * them — we only record %ds for the struct. Push order builds struct registers
 * (idt.h) bottom-up: ds is lowest, then r15..r8, edi/esi/ebp/esp/ebx/edx/ecx/eax. */
.type isr_common_stub, @function
isr_common_stub:                       /* on entry: top of stack = int_no       */
	push %rax
	push %rcx
	push %rdx
	push %rbx
	lea  32(%rsp), %rax            /* trap rsp (4 quads pushed so far) -> esp   */
	push %rax
	push %rbp
	push %rsi
	push %rdi
	push %r8
	push %r9
	push %r10
	push %r11
	push %r12
	push %r13
	push %r14
	push %r15
	xor  %rax, %rax
	mov  %ds, %ax
	push %rax                      /* ds slot                                  */

	cld                            /* user DF=1 would run the kernel's rep movs/stos backwards */
	mov  %rsp, %rdi                /* arg: pointer to struct registers         */
	call isr_handler

	/* DS is per CPU, not per task: a task resumed on a CPU that ran the kernel would iretq to ring 3 with
	 * DPL-0 DS, which the CPU nulls, and compatibility mode then faults on every DS access. */
	pop  %rax                      /* ds saved at entry                        */
	mov  %ax, %ds
	mov  %ax, %es
	pop  %r15
	pop  %r14
	pop  %r13
	pop  %r12
	pop  %r11
	pop  %r10
	pop  %r9
	pop  %r8
	pop  %rdi
	pop  %rsi
	pop  %rbp
	add  $8, %rsp                  /* drop the esp slot (not a real register)  */
	pop  %rbx
	pop  %rdx
	pop  %rcx
	pop  %rax
	add  $16, %rsp                 /* drop int_no and err_code                 */
	iretq
.size isr_common_stub, . - isr_common_stub
#else
.type isr_common_stub, @function
isr_common_stub:
	pusha                          /* edi,esi,ebp,esp,ebx,edx,ecx,eax        */

	mov %ds, %ax                   /* save the current data segment           */
	push %eax

	mov $0x10, %ax                 /* switch to the kernel data segment       */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs                   /* %gs stays: it is the user's TLS segment */

	cld
	push %esp                      /* arg: pointer to struct registers        */
	call isr_handler
	add $4, %esp

	pop %eax                       /* restore the original data segment       */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs

	popa
	add $8, %esp                   /* drop int_no and err_code                */
	iret                           /* pops eip, cs, eflags (restores IF)      */
.size isr_common_stub, . - isr_common_stub
#endif
