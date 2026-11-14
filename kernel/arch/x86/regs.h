/* regs.h: the register frame a trap leaves for C, and the system call ABI on top of it. */
#ifndef OWOS_REGS_H
#define OWOS_REGS_H

#include <stdbool.h>
#include <stdint.h>

/* Register state pushed by the stubs (isr.S / irq.S), in memory order
 * (low -> high). The common stubs pass a pointer to this frame.
 *
 * The 64-bit frame keeps the 32-bit field NAMES (eip, eax, esp, ...) so every
 * consumer compiles unchanged; they are just 64-bit-wide and hold the r*
 * values (eip=rip, eax=rax, ...). The added r8..r15 and the always-present
 * useresp/ss (long mode pushes SS:RSP on every interrupt, not only ring-3
 * traps) are the only structural differences. Field order MUST match the push
 * order in the 64-bit common stub. */
#ifdef __x86_64__
struct registers {
  uint64_t ds; /* saved data segment    */
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t edi, esi, ebp, esp, ebx, edx, ecx, eax; /* r* general registers  */
  uint64_t int_no, err_code;                       /* pushed by the stub    */
  uint64_t eip, cs, eflags;                        /* pushed by the CPU     */
  uint64_t useresp, ss;                            /* always pushed (64-bit)*/
};
#else
struct registers {
  uint32_t ds;                                     /* saved data segment    */
  uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; /* pusha                 */
  uint32_t int_no, err_code;                       /* pushed by the stub    */
  uint32_t eip, cs, eflags;                        /* pushed by the CPU     */
  uint32_t useresp, ss;                            /* only on a ring3 trap  */
};
#endif

/* System calls: number in eax, arguments in ebx/ecx/edx, result in eax. */
#define SC_NUM(r) ((r)->eax)
#define SC_ARG0(r) ((r)->ebx)
#define SC_ARG1(r) ((r)->ecx)
#define SC_ARG2(r) ((r)->edx)
#define SC_RET(r) ((r)->eax)

/* Whether the frame interrupted user code (ring 3). */
static inline bool regs_from_user(const struct registers *r) { return (r->cs & 3) == 3; }

#endif /* OWOS_REGS_H */
