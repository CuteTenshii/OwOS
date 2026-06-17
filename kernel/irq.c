/* irq.c — remap the 8259 PIC and dispatch hardware interrupts. */

#include <stdint.h>
#include "irq.h"
#include "idt.h"
#include "gdt.h"
#include "io.h"
#include "smp.h"

/* 8259 PIC ports. Two chips: a master and a slave cascaded onto IRQ2. */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define PIC_EOI    0x20   /* "end of interrupt" command                       */

#define ICW1_INIT  0x11   /* start init sequence, expect ICW4                 */
#define ICW4_8086  0x01   /* 8086/88 mode                                     */

#define IRQ_BASE   32     /* remapped master offset (slave at +8 = 40)        */

/* The 16 IRQ stubs from irq.S. */
extern void irq0(void),  irq1(void),  irq2(void),  irq3(void);
extern void irq4(void),  irq5(void),  irq6(void),  irq7(void);
extern void irq8(void),  irq9(void),  irq10(void), irq11(void);
extern void irq12(void), irq13(void), irq14(void), irq15(void);

static void (*const irq_stubs[16])(void) = {
	irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
	irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15,
};

static irq_handler_t handlers[16];

/* Reprogram the PICs so IRQ0..15 arrive on vectors 32..47 instead of
 * colliding with the CPU exception vectors. Leaves all lines masked. */
static void pic_remap(void)
{
	outb(PIC1_CMD, ICW1_INIT);  io_wait();
	outb(PIC2_CMD, ICW1_INIT);  io_wait();
	outb(PIC1_DATA, IRQ_BASE);     io_wait();   /* ICW2: master vector offset */
	outb(PIC2_DATA, IRQ_BASE + 8); io_wait();   /* ICW2: slave vector offset  */
	outb(PIC1_DATA, 0x04);      io_wait();      /* ICW3: slave is on IRQ2     */
	outb(PIC2_DATA, 0x02);      io_wait();      /* ICW3: slave cascade id     */
	outb(PIC1_DATA, ICW4_8086); io_wait();
	outb(PIC2_DATA, ICW4_8086); io_wait();

	outb(PIC1_DATA, 0xFF);                      /* mask every line for now    */
	outb(PIC2_DATA, 0xFF);
}

void irq_register(uint8_t irq, irq_handler_t handler)
{
	if (irq < 16)
		handlers[irq] = handler;
}

void irq_unmask(uint8_t irq)
{
	uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
	if (irq >= 8)
		irq -= 8;
	outb(port, inb(port) & ~(1 << irq));
}

/* Called from irq_common_stub for every hardware interrupt. */
void irq_handler(struct registers *r)
{
	/* The Local APIC timer (AP scheduler tick) shares this dispatch but acks the
	 * LAPIC, not the PIC. */
	if (r->int_no == LAPIC_TIMER_VEC) {
		smp_lapic_tick(r);
		return;
	}

	uint8_t irq = r->int_no - IRQ_BASE;

	/* Acknowledge the PIC *before* running the handler. A preemptive
	 * scheduler's timer handler context-switches away and never returns
	 * here, so a deferred EOI would strand the PIC. Sending it now is safe:
	 * the interrupt gate keeps IF=0, so nothing nests until iret. The slave
	 * needs its own EOI before the master. */
	if (irq >= 8)
		outb(PIC2_CMD, PIC_EOI);
	outb(PIC1_CMD, PIC_EOI);

	if (irq < 16 && handlers[irq])
		handlers[irq](r);
}

void irq_init(void)
{
	pic_remap();

	for (int i = 0; i < 16; i++)
		idt_set_gate(IRQ_BASE + i, (uint32_t)irq_stubs[i],
		             GDT_KERNEL_CODE, 0x8E);
}

/* Re-initialise the 8259 PIC after an ACPI S3 resume. The firmware's wake POST
 * remaps the PIC back to the legacy BIOS vectors and masks the lines, so a
 * timer tick would otherwise land on a CPU exception vector (or never fire).
 * Restore our master->0x20 / slave->0x28 mapping, then reinstate the saved line
 * masks. The IDT gates are unchanged (the kernel IDT is restored by lidt). */
void irq_resume(uint8_t mask1, uint8_t mask2)
{
	pic_remap();                 /* re-init; leaves all lines masked */
	outb(PIC1_DATA, mask1);      /* reinstate the masks from before sleep */
	outb(PIC2_DATA, mask2);
}
