/* idt.c: build and install the IDT, and dispatch CPU exceptions. */

#include "idt.h"
#include "console.h"
#include "crash.h"
#include "creg.h"
#include "fb.h"
#include "gdt.h"
#include "paging.h"
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include <stdint.h>

#ifdef __x86_64__
/* One 16-byte 64-bit interrupt gate descriptor. */
struct idt_entry {
  uint16_t base_low;  /* handler address bits  0..15                    */
  uint16_t selector;  /* code segment selector to run the handler in    */
  uint8_t ist;        /* interrupt-stack-table index (0 = none)         */
  uint8_t flags;      /* present, ring, gate type                       */
  uint16_t base_mid;  /* handler address bits 16..31                    */
  uint32_t base_high; /* handler address bits 32..63                    */
  uint32_t reserved;
} __attribute__((packed));
#else
/* One 8-byte interrupt gate descriptor. */
struct idt_entry {
  uint16_t base_low;  /* handler address bits  0..15                    */
  uint16_t selector;  /* code segment selector to run the handler in    */
  uint8_t always0;    /* reserved, always zero                          */
  uint8_t flags;      /* present, ring, gate type                       */
  uint16_t base_high; /* handler address bits 16..31                    */
} __attribute__((packed));
#endif

/* Operand for lidt; base is uintptr_t (8 bytes on 64-bit). */
struct idt_ptr {
  uint16_t limit;
  uintptr_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idt_ptr;

/* The 32 exception stubs from isr.S. */
extern void isr0(void), isr1(void), isr2(void), isr3(void);
extern void isr4(void), isr5(void), isr6(void), isr7(void);
extern void isr8(void), isr9(void), isr10(void), isr11(void);
extern void isr12(void), isr13(void), isr14(void), isr15(void);
extern void isr16(void), isr17(void), isr18(void), isr19(void);
extern void isr20(void), isr21(void), isr22(void), isr23(void);
extern void isr24(void), isr25(void), isr26(void), isr27(void);
extern void isr28(void), isr29(void), isr30(void), isr31(void);

static void (*const isr_stubs[32])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,  isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23, isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

static const char *const exception_messages[32] = {
    "Divide error",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "Bound range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "Reserved",
    "x87 floating-point exception",
    "Alignment check",
    "Machine check",
    "SIMD floating-point",
    "Virtualization exception",
    "Control protection",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor injection",
    "VMM communication",
    "Security exception",
    "Reserved",
};

void idt_set_gate(uint8_t num, uintptr_t base, uint16_t sel, uint8_t flags)
{
#ifdef __x86_64__
  idt[num].base_low = base & 0xFFFF;
  idt[num].base_mid = (base >> 16) & 0xFFFF;
  idt[num].base_high = (uint32_t)(base >> 32);
  idt[num].selector = sel;
  idt[num].ist = 0;
  idt[num].flags = flags;
  idt[num].reserved = 0;
#else
  idt[num].base_low = base & 0xFFFF;
  idt[num].base_high = (base >> 16) & 0xFFFF;
  idt[num].selector = sel;
  idt[num].always0 = 0;
  idt[num].flags = flags;
#endif
}

/* Panic-screen string builders: no libc or heap (it may be corrupt); return the end pointer. */
static char *p_str(char *p, const char *s)
{
  while (*s)
    *p++ = *s++;
  return p;
}
static char *p_hex(char *p, uint32_t v)
{
  const char *h = "0123456789ABCDEF";
  *p++ = '0';
  *p++ = 'x';
  for (int i = 28; i >= 0; i -= 4)
    *p++ = h[(v >> i) & 0xF];
  return p;
}

/* Is `a` a present page? No USER bit needed, so the panic handler can probe supervisor stacks. */
#ifdef __x86_64__
static bool addr_present(uint64_t a)
{
  uint64_t *t = (uint64_t *)(uintptr_t)((uint64_t)read_cr3() & ~0xFFFull);
  for (int lvl = 39; lvl >= 12; lvl -= 9) {
    uint64_t e = t[(a >> lvl) & 0x1FF];
    if (!(e & 1))
      return false;
    if ((e & 0x80) && lvl > 12) /* 1 GiB / 2 MiB large page: present */
      return true;
    t = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ull);
  }
  return true;
}
#else
static bool addr_present(uint32_t a)
{
  uint32_t *pd = (uint32_t *)((uint32_t)read_cr3() & ~0xFFFu);
  uint32_t pde = pd[a >> 22];
  if (!(pde & 1))
    return false;
  if (pde & 0x80) /* 4 MiB large page: present */
    return true;
  uint32_t *pt = (uint32_t *)(pde & ~0xFFFu);
  return pt[(a >> 12) & 0x3FF] & 1;
}
#endif

/* Crash report on the framebuffer (UEFI has no VGA text); no heap or locks, no-op without graphics. */
static void panic_screen(struct registers *r)
{
  if (!fb_active())
    return;

  int W = (int)fb_width(), H = (int)fb_height();
  uint32_t bg = fb_rgb(0x0a, 0x14, 0x6e);
  uint32_t fg = fb_rgb(0xe6, 0xe6, 0xe6);
  uint32_t hi = fb_rgb(0xff, 0xd0, 0x40);

  fb_fill_rect(0, 0, W, H, bg);

  int x = 24, y = 24, lh = 18;
  char line[160], *p;

  fb_text(x, y, "KERNEL PANIC", hi, bg);
  y += lh * 2;

  p = p_str(line, "exception: ");
  p = p_str(p, r->int_no < 32 ? exception_messages[r->int_no] : "Unknown");
  *p = 0;
  fb_text(x, y, line, fg, bg);
  y += lh;

  p = line;
  p = p_str(p, "vector=");
  p = p_hex(p, r->int_no);
  p = p_str(p, "  err=");
  p = p_hex(p, r->err_code);
  *p = 0;
  fb_text(x, y, line, fg, bg);
  y += lh;

  p = line;
  p = p_str(p, "eip=");
  p = p_hex(p, r->eip);
  p = p_str(p, "  cs=");
  p = p_hex(p, r->cs);
  p = p_str(p, "  eflags=");
  p = p_hex(p, r->eflags);
  *p = 0;
  fb_text(x, y, line, fg, bg);
  y += lh;

  if (r->int_no == 14) { /* page fault: CR2 holds the address */
    uint32_t cr2 = (uint32_t)read_cr2();
    uint32_t cr3 = (uint32_t)read_cr3();
    p = line;
    p = p_str(p, "cr2=");
    p = p_hex(p, cr2);
    p = p_str(p, "  cr3=");
    p = p_hex(p, cr3);
    *p = 0;
    fb_text(x, y, line, fg, bg);
    y += lh;

    p = line;
    p = p_str(p, "fault: ");
    p = p_str(p, (r->err_code & 1) ? "protection " : "not-present ");
    p = p_str(p, (r->err_code & 2) ? "write " : "read ");
    p = p_str(p, (r->err_code & 4) ? "user " : "supervisor ");
    if (r->err_code & 16)
      p = p_str(p, "insn-fetch ");
    *p = 0;
    fb_text(x, y, line, fg, bg);
    y += lh;
  }
  y += lh;

  p = line;
  p = p_str(p, "eax=");
  p = p_hex(p, r->eax);
  p = p_str(p, "  ebx=");
  p = p_hex(p, r->ebx);
  p = p_str(p, "  ecx=");
  p = p_hex(p, r->ecx);
  p = p_str(p, "  edx=");
  p = p_hex(p, r->edx);
  *p = 0;
  fb_text(x, y, line, fg, bg);
  y += lh;

  p = line;
  p = p_str(p, "esi=");
  p = p_hex(p, r->esi);
  p = p_str(p, "  edi=");
  p = p_hex(p, r->edi);
  p = p_str(p, "  ebp=");
  p = p_hex(p, r->ebp);
  p = p_str(p, "  esp=");
  p = p_hex(p, r->esp);
  *p = 0;
  fb_text(x, y, line, fg, bg);
  y += lh;

  /* TEMP: boot progress breadcrumb and current task for the wm-thread boot crash. */
  extern const char *g_boot_stage;
  p = line;
  p = p_str(p, "boot=");
  p = p_str(p, g_boot_stage ? g_boot_stage : "?");
  p = p_str(p, "  task=");
  p = p_str(p, sched_current_name());
  *p = 0;
  fb_text(x, y, line, fg, bg);
  y += lh;

  /* Dump words around ESP, probing each first (a corrupt ESP would double-fault). TEMP. */
  p = line;
  p = p_str(p, "stk:");
  for (int i = -2; i <= 5; i++) {
    uint32_t a = (uint32_t)r->esp + (uint32_t)(i * 4);
    p = p_str(p, " ");
    if (addr_present(a))
      p = p_hex(p, *(volatile uint32_t *)(uintptr_t)a);
    else
      p = p_str(p, "--------");
  }
  *p = 0;
  fb_text(x, y, line, fg, bg);
  y += lh * 2;

  fb_text(x, y, "system halted. photograph this screen.", hi, bg);

  fb_present();
}

/* Ring-3 exception to POSIX signal: #DE SIGFPE, #UD SIGILL, memory faults SIGSEGV. */
static int fault_signal(uint32_t vec)
{
  switch (vec) {
  case 0:
    return SIGFPE;
  case 6:
    return SIGILL;
  default:
    return SIGSEGV;
  }
}

/* Called from isr_common_stub for every CPU exception. */
void isr_handler(struct registers *r)
{
  /* NMI and #MC are not caused by the interrupted instruction, so they never become a signal to the user task. */
  if (r->int_no == 2)
    return;
  bool async = r->int_no == 18;
  int user_fault = (r->int_no == 0x80 || (r->int_no < 32 && !async)) && (r->cs & 3) == 3;
  if (user_fault)
    sched_kernel_enter();
  if (r->int_no == 0x80) {
    syscall_dispatch(r);
    sched_signal_deliver(r);
    sched_kernel_leave();
    return;
  }
  if (r->int_no == 14 && paging_handle_page_fault((uint32_t)read_cr2(), r->err_code)) {
    if (user_fault)
      sched_kernel_leave();
    return;
  }

  /* Ring-3 fault: deliver the signal if handled, else record a crash and kill the process. */
  if (user_fault) {
    if (sched_fault_signal(r, fault_signal(r->int_no), r->int_no == 14 ? (uint32_t)read_cr2() : 0)) {
      sched_kernel_leave();
      return;
    }
    crash_record(sched_current_name(), r);
    sched_exit(139);
  }

  con_set_color(CON_WHITE, CON_RED);
  con_printf("\n*** CPU EXCEPTION: %s ***\n", r->int_no < 32 ? exception_messages[r->int_no] : "Unknown");

  con_printf("  vector=0x%08X  err=0x%08X  eip=0x%08X", r->int_no, r->err_code, r->eip);

  if (r->int_no == 14) { /* page fault: CR2 holds the address */
    uint32_t cr2 = (uint32_t)read_cr2();
    con_printf("  cr2=0x%08X", cr2);
  }

  con_write("\n  system halted.\n");

  panic_screen(r);

  for (;;)
    __asm__ volatile("cli; hlt");
}

void idt_init(void)
{
  idt_ptr.limit = sizeof(idt) - 1;
  idt_ptr.base = (uintptr_t)&idt;

  /* 0x8E = present, ring 0, 32-bit interrupt gate (clears IF on entry). */
  for (int i = 0; i < 32; i++)
    idt_set_gate(i, (uintptr_t)isr_stubs[i], GDT_KERNEL_CODE, 0x8E);

  __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}

/* Load the (already-built, shared) IDT on the calling CPU, for an AP. */
void idt_load(void) { __asm__ volatile("lidt %0" : : "m"(idt_ptr)); }
