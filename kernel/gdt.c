/* gdt.c — build and install a flat 32-bit Global Descriptor Table. */

#include <stdint.h>
#include "gdt.h"

/* One 8-byte segment descriptor. The fields are scattered across the 8 bytes
 * for historical (16-bit -> 32-bit) reasons, hence the split base/limit. */
struct gdt_entry {
	uint16_t limit_low;    /* limit bits  0..15                              */
	uint16_t base_low;     /* base  bits  0..15                              */
	uint8_t  base_mid;     /* base  bits 16..23                              */
	uint8_t  access;       /* present, ring, type, etc.                      */
	uint8_t  granularity;  /* high nibble: flags; low nibble: limit 16..19   */
	uint8_t  base_high;    /* base  bits 24..31                              */
} __attribute__((packed));

/* Operand for the lgdt instruction: table size and linear address. */
struct gdt_ptr {
	uint16_t limit;        /* size of the table in bytes, minus one          */
	uint32_t base;         /* linear address of the first entry              */
} __attribute__((packed));

/* 32-bit Task State Segment. The CPU only reads ss0/esp0 from us (the kernel
 * stack to switch to when an interrupt enters ring 0 from ring 3); the rest
 * stays zero since we don't use hardware task switching. */
struct tss_entry {
	uint32_t prev;
	uint32_t esp0, ss0;
	uint32_t esp1, ss1, esp2, ss2;
	uint32_t cr3, eip, eflags;
	uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
	uint32_t es, cs, ss, ds, fs, gs, ldt;
	uint16_t trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr   gdt_ptr;
static struct tss_entry tss;

/* Defined in gdt_flush.S: lgdt the pointer, then reload the segment regs. */
extern void gdt_flush(uint32_t gdt_ptr_addr);

void tss_set_kernel_stack(unsigned int esp0)
{
	tss.esp0 = esp0;
}

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags)
{
	gdt[i].limit_low   = limit & 0xFFFF;
	gdt[i].base_low    = base & 0xFFFF;
	gdt[i].base_mid    = (base >> 16) & 0xFF;
	gdt[i].access      = access;
	gdt[i].granularity = ((limit >> 16) & 0x0F) | (flags & 0xF0);
	gdt[i].base_high   = (base >> 24) & 0xFF;
}

void gdt_init(void)
{
	/* access byte:  P DPL S | E DC RW A
	 *   0x9A = present, ring0, code+data type, executable, readable   (code)
	 *   0x92 = present, ring0, code+data type, data,       writable   (data)
	 * flags nibble:  G D/B L AVL
	 *   0xC = 4 KiB granularity, 32-bit segment
	 * limit 0xFFFFF with 4 KiB granularity => spans the full 4 GiB.   */
	gdt_set_entry(0, 0, 0,       0,    0);     /* 0x00: null descriptor      */
	gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC0);  /* 0x08: kernel code          */
	gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);  /* 0x10: kernel data          */
	gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xC0);  /* 0x18: user code   (DPL 3)  */
	gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);  /* 0x20: user data   (DPL 3)  */

	/* TSS descriptor: base = &tss, type 0x9 (32-bit TSS available). */
	tss.ss0 = GDT_KERNEL_DATA;
	tss.iomap_base = sizeof(tss);
	gdt_set_entry(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

	gdt_ptr.limit = sizeof(gdt) - 1;
	gdt_ptr.base  = (uint32_t)&gdt;

	gdt_flush((uint32_t)&gdt_ptr);

	__asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS));
}
