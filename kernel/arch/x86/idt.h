/* idt.h: Interrupt Descriptor Table setup.
 *
 * The IDT tells the CPU where to jump for each interrupt vector. We install
 * handlers for the 32 CPU exception vectors (0-31); the IRQ module adds gates
 * for the remapped hardware interrupts (32-47) on top.
 */
#ifndef OWOS_IDT_H
#define OWOS_IDT_H

#include "regs.h"
#include <stdint.h>

void idt_init(void);

/* An application processor loads the shared IDT on itself. */
void idt_load(void);

/* Install one IDT gate. Exposed so the IRQ module can add its own gates. */
void idt_set_gate(uint8_t num, uintptr_t base, uint16_t sel, uint8_t flags);

#endif /* OWOS_IDT_H */
