/* task.c: the x86 side of the scheduler (task_arch.h): initial task contexts, exec, TLS, signal frames. */
#include "crashrec.h"
#include "creg.h"
#include "fpu.h"
#include "gdt.h"
#include "kstring.h"
#include "loader.h"
#include "regs.h"
#include "syscall_nr.h"
#include "task_arch.h"

/* From switch_*.S. */
extern void thread_start(void);
extern void enter_user_task(void);
extern void fork_child_entry(void);

#define USER_CS (GDT_USER_CODE | 3)
#define USER_DS (GDT_USER_DATA | 3)
#define USER_FLAGS 0x202u /* IF set */

uintptr_t arch_kthread_init(uintptr_t kstack_top, void (*entry)(void))
{
  /* context_switch rets into thread_start, then `entry`, then thread_exit, past the zeroed callee-saved slots. */
#ifdef __x86_64__
  uint64_t *sp = (uint64_t *)kstack_top;
  *--sp = (uint64_t)(uintptr_t)thread_exit;
  *--sp = (uint64_t)(uintptr_t)entry;
  *--sp = (uint64_t)(uintptr_t)thread_start;
  for (int i = 0; i < 6; i++)
    *--sp = 0; /* rbp,rbx,r12-r15 */
#else
  uint32_t *sp = (uint32_t *)kstack_top;
  *--sp = (uint32_t)thread_exit;
  *--sp = (uint32_t)entry;
  *--sp = (uint32_t)thread_start;
  for (int i = 0; i < 4; i++)
    *--sp = 0; /* ebp,ebx,esi,edi */
#endif
  return (uintptr_t)sp;
}

uintptr_t arch_user_init(uintptr_t kstack_top, uaddr_t entry, uaddr_t user_sp)
{
  /* The first schedule rets into enter_user_task, which irets to `entry`. */
#ifdef __x86_64__
  uint64_t *sp = (uint64_t *)kstack_top;
  *--sp = USER_DS;
  *--sp = user_sp;
  *--sp = USER_FLAGS;
  *--sp = USER_CS;
  *--sp = entry;
  *--sp = (uint64_t)(uintptr_t)enter_user_task;
  for (int i = 0; i < 6; i++)
    *--sp = 0; /* rbp,rbx,r12-r15 */
#else
  uint32_t *sp = (uint32_t *)kstack_top;
  *--sp = USER_DS;
  *--sp = user_sp;
  *--sp = USER_FLAGS;
  *--sp = USER_CS;
  *--sp = entry;
  *--sp = (uint32_t)enter_user_task;
  for (int i = 0; i < 4; i++)
    *--sp = 0; /* ebp,ebx,esi,edi */
#endif
  return (uintptr_t)sp;
}

uintptr_t arch_fork_init(uintptr_t kstack_top, const struct registers *parent)
{
  /* The parent's trap frame at the top of the stack with eax = 0, under a context_switch frame that rets into
   * fork_child_entry, the tail of the trap return path. */
  struct registers *cf = (struct registers *)(kstack_top - sizeof *cf);
  *cf = *parent;
  cf->eax = 0;
#ifdef __x86_64__
  uint64_t *sp = (uint64_t *)cf;
  *--sp = (uint64_t)(uintptr_t)fork_child_entry;
  for (int i = 0; i < 6; i++)
    *--sp = 0; /* rbp,rbx,r12-r15 */
#else
  uint32_t *sp = (uint32_t *)cf;
  *--sp = (uint32_t)fork_child_entry;
  for (int i = 0; i < 4; i++)
    *--sp = 0; /* ebp,ebx,esi,edi */
#endif
  return (uintptr_t)sp;
}

void arch_exec_regs(struct registers *r, uaddr_t entry, uaddr_t user_sp)
{
  r->eip = entry;
  r->cs = USER_CS;
  r->eflags = USER_FLAGS;
  r->useresp = user_sp;
  r->ss = USER_DS;
  r->ds = USER_DS;
  r->eax = r->ebx = r->ecx = r->edx = 0;
  r->esi = r->edi = r->ebp = 0;
#ifdef __x86_64__
  r->r8 = r->r9 = r->r10 = r->r11 = r->r12 = r->r13 = r->r14 = r->r15 = 0;
#endif
}

void arch_set_kernel_stack(uintptr_t kstack_top) { tss_set_kernel_stack(kstack_top); }

void arch_set_tls(uaddr_t base) { gdt_set_tls(base); }

#ifdef __x86_64__
/* Handler frame on the user stack. The handler takes sig, &info and &uc in rdi/rsi/rdx and returns through
 * `pretcode` into `tramp` (SYS_SIGRETURN). The FPU image rides along: compiled code keeps live values in xmm
 * registers, which the handler is free to clobber. */
struct sigframe {
  uint64_t pretcode;      /* -> tramp */
  uint64_t saved_blocked; /* mask to restore on sigreturn */
  struct k_sigctx uc;     /* interrupted context to resume */
  struct k_siginfo info;
  uint8_t tramp[8]; /* mov $SYS_SIGRETURN,%eax ; int $0x80 */
  uint8_t fpu[FPU_AREA_BYTES];
};
#define SIG_RED_ZONE 128 /* the SysV area below rsp that the interrupted code may still be using */
#else
/* Handler frame on the user stack: [pretcode][sig][&info][&uc], the arguments of an SA_SIGINFO handler (a
 * one-argument handler ignores the last two); pretcode points at `tramp` (SYS_SIGRETURN). */
struct sigframe {
  uint32_t pretcode; /* -> tramp */
  uint32_t sig;      /* handler arguments: sig, info, ctx */
  uint32_t pinfo;
  uint32_t puc;
  uint32_t saved_blocked; /* mask to restore on sigreturn */
  struct k_sigctx uc;     /* interrupted context to resume */
  struct k_siginfo info;
  uint8_t tramp[8]; /* mov $SYS_SIGRETURN,%eax ; int $0x80 */
};
#define SIG_RED_ZONE 0
#endif

int arch_sigframe_setup(struct registers *r, const struct k_siginfo *info, uint32_t blocked, uaddr_t handler)
{
  uint64_t sp = r->useresp;
  if (sp > USER_REGION_END || sp < USER_BASE + SIG_RED_ZONE + sizeof(struct sigframe) + 16)
    return -1;
  uint64_t base = sp - SIG_RED_ZONE - sizeof(struct sigframe);
#ifdef __x86_64__
  base = (base & ~15ull) - 8; /* a function is entered with rsp + 8 16-byte aligned */
#else
  base &= ~3ull;
#endif
  if (base < USER_BASE)
    return -1;

  struct sigframe f;
  memset(&f, 0, sizeof f); /* padding must not carry kernel stack bytes to the user */
  f.pretcode = (uint32_t)base + (uint32_t)__builtin_offsetof(struct sigframe, tramp);
  f.saved_blocked = blocked;
  f.info = *info;
  f.uc.eip = r->eip;
  f.uc.eflags = r->eflags;
  f.uc.esp = r->useresp;
  f.uc.eax = r->eax;
  f.uc.ecx = r->ecx;
  f.uc.edx = r->edx;
  f.uc.ebx = r->ebx;
  f.uc.ebp = r->ebp;
  f.uc.esi = r->esi;
  f.uc.edi = r->edi;
#ifdef __x86_64__
  f.uc.r8 = r->r8;
  f.uc.r9 = r->r9;
  f.uc.r10 = r->r10;
  f.uc.r11 = r->r11;
  f.uc.r12 = r->r12;
  f.uc.r13 = r->r13;
  f.uc.r14 = r->r14;
  f.uc.r15 = r->r15;
  uint8_t fxbuf[FPU_AREA_BYTES + FPU_AREA_ALIGN - 1];
  uint8_t *fx = (uint8_t *)(((uintptr_t)fxbuf + FPU_AREA_ALIGN - 1) & ~(uintptr_t)(FPU_AREA_ALIGN - 1));
  fpu_save(fx); /* the task's own state: the kernel never touches the FPU */
  memcpy(f.fpu, fx, sizeof f.fpu);
#else
  f.sig = (uint32_t)info->signo;
  f.pinfo = (uint32_t)base + (uint32_t)__builtin_offsetof(struct sigframe, info);
  f.puc = (uint32_t)base + (uint32_t)__builtin_offsetof(struct sigframe, uc);
#endif
  f.tramp[0] = 0xB8; /* mov $imm32, %eax */
  f.tramp[1] = (uint8_t)(SYS_SIGRETURN);
  f.tramp[2] = (uint8_t)(SYS_SIGRETURN >> 8);
  f.tramp[3] = (uint8_t)(SYS_SIGRETURN >> 16);
  f.tramp[4] = (uint8_t)(SYS_SIGRETURN >> 24);
  f.tramp[5] = 0xCD; /* int $0x80 */
  f.tramp[6] = 0x80;
  f.tramp[7] = 0x90; /* nop pad */
  /* The user's esp may point anywhere: an unmapped or read-only stack must fail, not fault in ring 0. */
  if (copyout((uint32_t)base, &f, sizeof f) != 0)
    return -1;

  r->useresp = (uintptr_t)base;
  r->eip = handler;
#ifdef __x86_64__
  r->edi = (uint32_t)info->signo;
  r->esi = base + __builtin_offsetof(struct sigframe, info);
  r->edx = base + __builtin_offsetof(struct sigframe, uc);
  fpu_load_init(); /* the handler starts from a clean FPU, with the default SSE modes */
#endif
  return 0;
}

int arch_sigreturn(struct registers *r, uint32_t *blocked)
{
  /* The handler's ret popped pretcode, so the frame starts one word below the stack pointer. */
  uint64_t base = r->useresp - sizeof(uintptr_t);
  if (r->useresp < USER_BASE + sizeof(uintptr_t) || base + sizeof(struct sigframe) > USER_REGION_END)
    return -1;
  struct sigframe f;
  if (copyin(&f, (uint32_t)base, sizeof f) != 0)
    return -1;
#ifdef __x86_64__
  /* iretq to a non-canonical rip faults in ring 0, so a rewritten context must stay in the lower half. */
  if ((f.uc.eip | f.uc.esp) >> 47)
    return -1;
#endif

  *blocked = (uint32_t)f.saved_blocked;
  r->eip = f.uc.eip;
  r->useresp = f.uc.esp;
  /* Sanitize user flags: keep arithmetic/DF/TF, force IF and bit1; never IOPL/NT. */
  r->eflags = (f.uc.eflags & 0x00000DD5u) | 0x00000202u;
  r->eax = f.uc.eax;
  r->ecx = f.uc.ecx;
  r->edx = f.uc.edx;
  r->ebx = f.uc.ebx;
  r->ebp = f.uc.ebp;
  r->esi = f.uc.esi;
  r->edi = f.uc.edi;
#ifdef __x86_64__
  r->r8 = f.uc.r8;
  r->r9 = f.uc.r9;
  r->r10 = f.uc.r10;
  r->r11 = f.uc.r11;
  r->r12 = f.uc.r12;
  r->r13 = f.uc.r13;
  r->r14 = f.uc.r14;
  r->r15 = f.uc.r15;
  uint8_t fxbuf[FPU_AREA_BYTES + FPU_AREA_ALIGN - 1];
  uint8_t *fx = (uint8_t *)(((uintptr_t)fxbuf + FPU_AREA_ALIGN - 1) & ~(uintptr_t)(FPU_AREA_ALIGN - 1));
  memcpy(fx, f.fpu, sizeof f.fpu);
  fpu_sanitize(fx);
  fpu_restore(fx);
#endif
  return 0;
}

uaddr_t arch_crash_capture(struct crash_rec *c, const struct registers *r)
{
  c->vector = (unsigned)r->int_no;
  c->err = (unsigned)r->err_code;
  c->page_fault = r->int_no == 14;
  c->addr = c->page_fault ? (uaddr_t)read_cr2() : 0;
  c->pc = r->eip;
  c->sp = r->useresp; /* r->esp is the kernel-stack value pushed on entry, not the faulting user's */
  const sys_word regs[CRASH_REGS] = {r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp, r->eflags};
  for (int i = 0; i < CRASH_REGS; i++)
    c->regs[i] = regs[i];
  return r->ebp;
}

const char *arch_trap_name(unsigned vector)
{
  return vector == 13   ? "general protection fault"
         : vector == 6  ? "invalid opcode"
         : vector == 0  ? "divide error"
         : vector == 1  ? "debug"
         : vector == 3  ? "breakpoint"
         : vector == 8  ? "double fault"
         : vector == 12 ? "stack fault"
         : vector == 16 ? "x87 floating-point error"
         : vector == 19 ? "SIMD floating-point error"
                        : "exception";
}
