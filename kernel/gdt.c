/* gdt.c — build and install the Global Descriptor Table (32-bit flat, or the
 * 64-bit long-mode layout). Segmentation does nothing useful in either mode;
 * the GDT exists for the ring0/ring3 code/data selectors and the per-CPU TSS
 * the CPU reads on a ring3->ring0 trap. */

#include "gdt.h"
#include "smp.h"
#include <stdint.h>

/* One 8-byte segment descriptor (kernel/user code+data). The fields are
 * scattered across the 8 bytes for historical (16-bit -> 32-bit) reasons. In
 * long mode base/limit are ignored for code/data; only the access/flag bits
 * (present, DPL, type, and the code segment's L bit) matter. */
struct gdt_entry {
  uint16_t limit_low;  /* limit bits  0..15                              */
  uint16_t base_low;   /* base  bits  0..15                              */
  uint8_t base_mid;    /* base  bits 16..23                              */
  uint8_t access;      /* present, ring, type, etc.                      */
  uint8_t granularity; /* high nibble: flags; low nibble: limit 16..19   */
  uint8_t base_high;   /* base  bits 24..31                              */
} __attribute__((packed));

/* Operand for the lgdt instruction: table size and linear address. base is
 * uintptr_t so the operand is the right width on both arches (32-bit lgdt reads
 * a 4-byte base; 64-bit lgdt reads an 8-byte base). */
struct gdt_ptr {
  uint16_t limit; /* size of the table in bytes, minus one          */
  uintptr_t base; /* linear address of the first entry              */
} __attribute__((packed));

/* Defined in gdt_flush.S: lgdt the pointer, then reload the segment regs. */
extern void gdt_flush(uintptr_t gdt_ptr_addr);

static void gdt_set_entry(struct gdt_entry *e, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
  e->limit_low = limit & 0xFFFF;
  e->base_low = base & 0xFFFF;
  e->base_mid = (base >> 16) & 0xFF;
  e->access = access;
  e->granularity = ((limit >> 16) & 0x0F) | (flags & 0xF0);
  e->base_high = (base >> 24) & 0xFF;
}

#ifdef __x86_64__
/* 64-bit Task State Segment. The CPU reads rsp0 (the kernel stack to switch to
 * on a ring3->ring0 trap) and the IST entries; the rest stays zero. */
struct tss_entry {
  uint32_t reserved0;
  uint64_t rsp0, rsp1, rsp2;
  uint64_t reserved1;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iomap_base;
} __attribute__((packed));

/* A 64-bit TSS descriptor is a system descriptor occupying TWO GDT slots (16
 * bytes): the usual 8 bytes plus base[63:32] and a reserved dword. */
struct tss_desc {
  struct gdt_entry lo;
  uint32_t base_upper; /* base bits 32..63                               */
  uint32_t reserved;
} __attribute__((packed));

/* Each TSS descriptor is 16 bytes = 2 GDT entries; TSS i starts at GDT entry
 * 5 + 2*i, selector TSS_SEL(i). TSS_SEL(0) == GDT_TSS (0x28). */
#define TSS_SEL(cpu) ((uint16_t)((5 + 2 * (cpu)) << 3))

static struct gdt_entry gdt[5 + 2 * SMP_MAX_CPUS];
static struct gdt_ptr gdt_ptr;
static struct tss_entry tss[SMP_MAX_CPUS];

void tss_set_kernel_stack(unsigned int rsp0) { tss[smp_this_cpu()].rsp0 = rsp0; }

static void gdt_set_tss(int idx, const struct tss_entry *t)
{
  struct tss_desc d = {0};
  uintptr_t base = (uintptr_t)t;
  uint32_t limit = sizeof(*t) - 1;
  /* type 0x9 = available 64-bit TSS, present, DPL 0; byte granularity. */
  gdt_set_entry(&d.lo, (uint32_t)base, limit, 0x89, 0x00);
  d.base_upper = (uint32_t)(base >> 32);
  *(struct tss_desc *)&gdt[idx] = d;
}

void gdt_init(void)
{
  /* flags nibble 0xA0 = G=1, L=1 (64-bit code). Data segments ignore most
   * fields in long mode; keep them flat for the rare legacy reader. */
  gdt_set_entry(&gdt[0], 0, 0, 0, 0);             /* 0x00: null            */
  gdt_set_entry(&gdt[1], 0, 0xFFFFF, 0x9A, 0xA0); /* 0x08: kernel code     */
  gdt_set_entry(&gdt[2], 0, 0xFFFFF, 0x92, 0xC0); /* 0x10: kernel data     */
  /* User code is 32-bit compat (G=1,D=1 = 0xC0), not L=1: user programs are
   * always 32-bit ELF, so iretq into 0x1B drops the CPU to compatibility mode.
   */
  gdt_set_entry(&gdt[3], 0, 0xFFFFF, 0xFA, 0xC0); /* 0x18: user code (DPL3)*/
  gdt_set_entry(&gdt[4], 0, 0xFFFFF, 0xF2, 0xC0); /* 0x20: user data (DPL3)*/

  for (int i = 0; i < SMP_MAX_CPUS; i++) {
    tss[i].iomap_base = sizeof(tss[i]);
    gdt_set_tss(5 + 2 * i, &tss[i]);
  }

  gdt_ptr.limit = sizeof(gdt) - 1;
  gdt_ptr.base = (uintptr_t)&gdt;

  gdt_flush((uintptr_t)&gdt_ptr);

  __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS)); /* BSP = TSS_SEL(0) */
}

#else
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
 * and GDT descriptor. The fixed 5 descriptors (null + kernel/user code/data)
 * are shared; TSS i sits at descriptor 5+i, selector TSS_SEL(i). */
#define TSS_SEL(cpu) ((uint16_t)((5 + (cpu)) << 3))

static struct gdt_entry gdt[5 + SMP_MAX_CPUS];
static struct gdt_ptr gdt_ptr;
static struct tss_entry tss[SMP_MAX_CPUS];

void tss_set_kernel_stack(unsigned int esp0) { tss[smp_this_cpu()].esp0 = esp0; }

void gdt_init(void)
{
  /* access byte:  P DPL S | E DC RW A
   *   0x9A = present, ring0, code+data type, executable, readable   (code)
   *   0x92 = present, ring0, code+data type, data,       writable   (data)
   * flags nibble:  G D/B L AVL
   *   0xC = 4 KiB granularity, 32-bit segment
   * limit 0xFFFFF with 4 KiB granularity => spans the full 4 GiB.   */
  gdt_set_entry(&gdt[0], 0, 0, 0, 0);             /* 0x00: null descriptor */
  gdt_set_entry(&gdt[1], 0, 0xFFFFF, 0x9A, 0xC0); /* 0x08: kernel code     */
  gdt_set_entry(&gdt[2], 0, 0xFFFFF, 0x92, 0xC0); /* 0x10: kernel data     */
  gdt_set_entry(&gdt[3], 0, 0xFFFFF, 0xFA, 0xC0); /* 0x18: user code (DPL3)*/
  gdt_set_entry(&gdt[4], 0, 0xFFFFF, 0xF2, 0xC0); /* 0x20: user data (DPL3)*/

  /* One 32-bit-TSS descriptor (type 0x9) per possible CPU. gdt_init runs before
   * smp_init, so the count isn't known yet — set all of them up; the unused
   * ones are simply never loaded. */
  for (int i = 0; i < SMP_MAX_CPUS; i++) {
    tss[i].ss0 = GDT_KERNEL_DATA;
    tss[i].iomap_base = sizeof(tss[i]);
    gdt_set_entry(&gdt[5 + i], (uint32_t)&tss[i], sizeof(tss[i]) - 1, 0x89, 0x00);
  }

  gdt_ptr.limit = sizeof(gdt) - 1;
  gdt_ptr.base = (uintptr_t)&gdt;

  gdt_flush((uintptr_t)&gdt_ptr);

  __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS)); /* BSP = TSS_SEL(0) */
}
#endif

/* An AP arrives on the trampoline's minimal GDT; switch it to the real kernel
 * GDT (which carries the per-CPU TSS descriptors) before it can ltr its own
 * TSS. */
void gdt_load_ap(void) { gdt_flush((uintptr_t)&gdt_ptr); }

/* Load this AP's task register with its own TSS (the BSP does it in gdt_init).
 */
void tss_load_ap(void) { __asm__ volatile("ltr %0" : : "r"(TSS_SEL(smp_this_cpu()))); }
