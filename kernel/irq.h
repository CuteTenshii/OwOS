/* irq.h — hardware interrupt (IRQ) handling on top of the 8259 PIC.
 *
 * irq_init() remaps the PIC away from the CPU exception vectors, installs the
 * IRQ stubs into the IDT, and leaves every line masked. Drivers call
 * irq_register() to claim a line and irq_unmask() to start receiving it.
 */
#ifndef OWOS_IRQ_H
#define OWOS_IRQ_H

#include <stdint.h>
#include "idt.h"   /* struct registers */

typedef void (*irq_handler_t)(struct registers *r);

void irq_init(void);
void irq_register(uint8_t irq, irq_handler_t handler);
void irq_unmask(uint8_t irq);

#endif /* OWOS_IRQ_H */
