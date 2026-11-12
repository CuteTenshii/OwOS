/* isr.S: interrupt service routine stubs for the 32 CPU exceptions.
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
