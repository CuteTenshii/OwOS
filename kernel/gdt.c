/* gdt.c — build and install a flat 32-bit Global Descriptor Table. */

#include <stdint.h>
#include "gdt.h"
#include "smp.h"

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

/* Per-CPU TSS: each core reads its own ss0/esp0, so each needs a distinct TSS
 * and GDT descriptor. The fixed 5 descriptors (null + kernel/user code/data) are
 * shared; TSS i sits at descriptor 5+i, selector TSS_SEL(i). */
#define TSS_SEL(cpu)  ((uint16_t)((5 + (cpu)) << 3))

static struct gdt_entry gdt[5 + SMP_MAX_CPUS];
static struct gdt_ptr   gdt_ptr;
static struct tss_entry tss[SMP_MAX_CPUS];

/* Defined in gdt_flush.S: lgdt the pointer, then reload the segment regs. */
extern void gdt_flush(uint32_t gdt_ptr_addr);

void tss_set_kernel_stack(unsigned int esp0)
{
	tss[smp_this_cpu()].esp0 = esp0;
}

/* An AP arrives on the trampoline's minimal GDT; switch it to the real kernel GDT
 * (which carries the per-CPU TSS descriptors) before it can ltr its own TSS. */
void gdt_load_ap(void)
{
	gdt_flush((uint32_t)&gdt_ptr);
}

/* Load this AP's task register with its own TSS (the BSP does it in gdt_init). */
void tss_load_ap(void)
{
	__asm__ volatile ("ltr %0" : : "r"(TSS_SEL(smp_this_cpu())));
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

	/* One 32-bit-TSS descriptor (type 0x9) per possible CPU. gdt_init runs before
	 * smp_init, so the count isn't known yet — set all of them up; the unused ones
	 * are simply never loaded. */
	for (int i = 0; i < SMP_MAX_CPUS; i++) {
		tss[i].ss0 = GDT_KERNEL_DATA;
		tss[i].iomap_base = sizeof(tss[i]);
		gdt_set_entry(5 + i, (uint32_t)&tss[i], sizeof(tss[i]) - 1, 0x89, 0x00);
	}

	gdt_ptr.limit = sizeof(gdt) - 1;
	gdt_ptr.base  = (uint32_t)&gdt;

	gdt_flush((uint32_t)&gdt_ptr);

	__asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS));   /* BSP = TSS_SEL(0) */
}
