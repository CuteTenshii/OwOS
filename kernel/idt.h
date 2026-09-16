/* idt.h — Interrupt Descriptor Table setup.
 *
 * The IDT tells the CPU where to jump for each interrupt vector. We install
 * handlers for the 32 CPU exception vectors (0-31); the IRQ module adds gates
 * for the remapped hardware interrupts (32-47) on top.
 */
#ifndef OWOS_IDT_H
#define OWOS_IDT_H

#include <stdint.h>

/* Register state pushed by the stubs (isr.S / irq.S), in memory order
 * (low -> high). The common stubs pass a pointer to this frame.
 *
 * The 64-bit frame keeps the 32-bit field NAMES (eip, eax, esp, ...) so every
 * consumer compiles unchanged — they are just 64-bit-wide and hold the r*
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

void idt_init(void);

/* An application processor loads the shared IDT on itself. */
void idt_load(void);

/* Install one IDT gate. Exposed so the IRQ module can add its own gates. */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);

#endif /* OWOS_IDT_H */
