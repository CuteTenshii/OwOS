/* irq_stubs.S: stubs for the 16 hardware IRQs, remapped to vectors 32-47.
 *
 * IRQs never push an error code, so every stub pushes a dummy zero (to match
 * struct registers) followed by its vector number, then falls into a common
 * stub that mirrors isr.S but dispatches to irq_handler.
 */

.section .text

.macro IRQ num, vec
.global irq\num
.type irq\num, @function
irq\num:
	push $0                        /* dummy error code                        */
	push $\vec                     /* vector number (32 + IRQ)                */
	jmp irq_common_stub
.endm

IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* Local APIC timer (the per-core scheduler tick on APs), vector 0x40. Same
 * frame shape as an IRQ; irq_handler special-cases it (LAPIC EOI, not PIC). */
.global lapic_timer_isr
.type lapic_timer_isr, @function
lapic_timer_isr:
	push $0
	push $0x40
	jmp irq_common_stub
