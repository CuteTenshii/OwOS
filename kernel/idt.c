/* idt.c — build and install the IDT, and dispatch CPU exceptions. */

#include <stdint.h>
#include "idt.h"
#include "gdt.h"
#include "vga.h"
#include "syscall.h"

/* One 8-byte interrupt gate descriptor. */
struct idt_entry {
	uint16_t base_low;     /* handler address bits  0..15                    */
	uint16_t selector;     /* code segment selector to run the handler in    */
	uint8_t  always0;      /* reserved, always zero                          */
	uint8_t  flags;        /* present, ring, gate type                       */
	uint16_t base_high;    /* handler address bits 16..31                    */
} __attribute__((packed));

/* Operand for lidt. */
struct idt_ptr {
	uint16_t limit;
	uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idt_ptr;

/* The 32 exception stubs from isr.S. */
extern void isr0(void),  isr1(void),  isr2(void),  isr3(void);
extern void isr4(void),  isr5(void),  isr6(void),  isr7(void);
extern void isr8(void),  isr9(void),  isr10(void), isr11(void);
extern void isr12(void), isr13(void), isr14(void), isr15(void);
extern void isr16(void), isr17(void), isr18(void), isr19(void);
extern void isr20(void), isr21(void), isr22(void), isr23(void);
extern void isr24(void), isr25(void), isr26(void), isr27(void);
extern void isr28(void), isr29(void), isr30(void), isr31(void);

static void (*const isr_stubs[32])(void) = {
	isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
	isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
	isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
	isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

static const char *const exception_messages[32] = {
	"Divide error", "Debug", "Non-maskable interrupt", "Breakpoint",
	"Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
	"Double fault", "Coprocessor segment overrun", "Invalid TSS", "Segment not present",
	"Stack-segment fault", "General protection fault", "Page fault", "Reserved",
	"x87 floating-point exception", "Alignment check", "Machine check", "SIMD floating-point",
	"Virtualization exception", "Control protection", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"Hypervisor injection", "VMM communication", "Security exception", "Reserved",
};

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
	idt[num].base_low  = base & 0xFFFF;
	idt[num].base_high = (base >> 16) & 0xFFFF;
	idt[num].selector  = sel;
	idt[num].always0   = 0;
	idt[num].flags     = flags;
}

/* Called from isr_common_stub for every CPU exception. */
void isr_handler(struct registers *r)
{
	if (r->int_no == 0x80) {       /* system call, not an exception            */
		syscall_dispatch(r);
		return;
	}

	vga_set_color(VGA_WHITE, VGA_RED);
	vga_write("\n*** CPU EXCEPTION: ");
	vga_write(r->int_no < 32 ? exception_messages[r->int_no] : "Unknown");
	vga_write(" ***\n");

	vga_write("  vector="); vga_write_hex(r->int_no);
	vga_write("  err=");    vga_write_hex(r->err_code);
	vga_write("  eip=");    vga_write_hex(r->eip);

	if (r->int_no == 14) {         /* page fault: CR2 holds the bad address    */
		uint32_t cr2;
		__asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
		vga_write("  cr2="); vga_write_hex(cr2);
	}

	vga_write("\n  system halted.\n");

	for (;;)
		__asm__ volatile ("cli; hlt");
}

void idt_init(void)
{
	idt_ptr.limit = sizeof(idt) - 1;
	idt_ptr.base  = (uint32_t)&idt;

	/* 0x8E = present, ring 0, 32-bit interrupt gate (clears IF on entry). */
	for (int i = 0; i < 32; i++)
		idt_set_gate(i, (uint32_t)isr_stubs[i], GDT_KERNEL_CODE, 0x8E);

	__asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}
