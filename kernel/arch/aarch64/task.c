/* task.c: the AArch64 side of the scheduler (task_arch.h): initial task contexts, exec, TLS, signal frames and
 * crash capture. */
#include "crashrec.h"
#include "exc.h"
#include "fpu.h"
#include "kstring.h"
#include "loader.h"
#include "regs.h"
#include "syscall_nr.h"
#include "task_arch.h"

/* From switch.S. */
extern void thread_start(void);
extern void enter_user_task(void);
extern void fork_child_entry(void);

#define CTX_WORDS 12 /* x19..x30, as context_switch saves them */
#define CTX_X19 0
#define CTX_X20 1
#define CTX_X21 2
#define CTX_X30 11

/* Kernel stacks come from the heap, which aligns to 8 bytes; AArch64 wants sp 16-byte aligned. */
static uintptr_t align_down16(uintptr_t a) { return a & ~(uintptr_t)15; }

static uint64_t *new_context(uintptr_t below, void (*resume)(void))
{
  uint64_t *ctx = (uint64_t *)(below - CTX_WORDS * sizeof(uint64_t));
  memset(ctx, 0, CTX_WORDS * sizeof(uint64_t));
  ctx[CTX_X30] = (uint64_t)(uintptr_t)resume;
  return ctx;
}

uintptr_t arch_kthread_init(uintptr_t kstack_top, void (*entry)(void))
{
  uint64_t *ctx = new_context(align_down16(kstack_top), thread_start);
  ctx[CTX_X19] = (uint64_t)(uintptr_t)entry;
  return (uintptr_t)ctx;
}

uintptr_t arch_user_init(uintptr_t kstack_top, uaddr_t entry, uaddr_t user_sp)
{
  uintptr_t top = align_down16(kstack_top);
  uint64_t *ctx = new_context(top, enter_user_task);
  ctx[CTX_X19] = entry;
  ctx[CTX_X20] = user_sp;
  ctx[CTX_X21] = top;
  return (uintptr_t)ctx;
}

uintptr_t arch_fork_init(uintptr_t kstack_top, const struct registers *parent)
{
  struct registers *cf = (struct registers *)(align_down16(kstack_top) - sizeof *cf);
  *cf = *parent;
  cf->x[0] = 0;
  uint64_t *ctx = new_context((uintptr_t)cf, fork_child_entry);
  ctx[CTX_X19] = (uint64_t)(uintptr_t)cf;
  return (uintptr_t)ctx;
}

void arch_exec_regs(struct registers *r, uaddr_t entry, uaddr_t user_sp)
{
  memset(r->x, 0, sizeof r->x);
  r->sp = user_sp;
  r->pc = entry;
  r->pstate = 0; /* EL0t, interrupts unmasked */
}

/* A trap from EL0 lands on SP_EL1 as the task's own eret left it, so there is nothing to switch. */
void arch_set_kernel_stack(uintptr_t kstack_top) { (void)kstack_top; }

void arch_set_tls(uaddr_t base) { __asm__ volatile("msr tpidr_el0, %0" : : "r"(base)); }

#define PSTATE_NZCV 0xF0000000u
#define FPCR_CONTROL 0x07C09F00u /* AHP DN FZ RMode, and the exception trap enables */
#define FPSR_STATUS 0xF800009Fu  /* NZCV QC, and the cumulative exception flags */

/* Handler frame on the user stack. The handler takes sig, &info and &uc in x0..x2 and returns through x30 into
 * `tramp` (SYS_SIGRETURN); the FP/SIMD registers ride along, since compiled code keeps live values in them. */
struct sigframe {
  struct k_siginfo info;
  uint64_t saved_blocked; /* mask to restore on sigreturn */
  struct k_sigctx uc;     /* interrupted context to resume */
  uint8_t fpu[FPU_AREA_BYTES];
  uint32_t tramp[2]; /* movz x8, #SYS_SIGRETURN ; svc #0 */
};

/* The trampoline is written as data and then run: clean it to the point of unification and drop any stale copy from
 * the instruction cache, through its user address in the current address space. */
static void sync_icache(uaddr_t va)
{
  __asm__ volatile("dc cvau, %0\n\tdsb ish\n\tic ivau, %0\n\tdsb ish\n\tisb" : : "r"(va) : "memory");
}

int arch_sigframe_setup(struct registers *r, const struct k_siginfo *info, uint32_t blocked, uaddr_t handler)
{
  uaddr_t sp = r->sp;
  if (sp > USER_REGION_END || sp < USER_BASE + sizeof(struct sigframe) + 16)
    return -1;
  uaddr_t base = (sp - sizeof(struct sigframe)) & ~(uaddr_t)15;

  struct sigframe f;
  memset(&f, 0, sizeof f); /* padding must not carry kernel stack bytes to the user */
  f.info = *info;
  f.saved_blocked = blocked;
  memcpy(f.uc.x, r->x, sizeof f.uc.x);
  f.uc.sp = r->sp;
  f.uc.pc = r->pc;
  f.uc.pstate = r->pstate;
  uint8_t fpbuf[FPU_AREA_BYTES + FPU_AREA_ALIGN - 1];
  uint8_t *fp = (uint8_t *)(((uintptr_t)fpbuf + FPU_AREA_ALIGN - 1) & ~(uintptr_t)(FPU_AREA_ALIGN - 1));
  fpu_save(fp); /* the task's own state: the kernel never touches FP/SIMD */
  memcpy(f.fpu, fp, sizeof f.fpu);
  f.tramp[0] = 0xD2800000u | ((uint32_t)SYS_SIGRETURN << 5) | 8u; /* movz x8, #SYS_SIGRETURN */
  f.tramp[1] = 0xD4000001u;                                       /* svc #0 */
  /* The user's sp may point anywhere: an unmapped or read-only stack must fail, not fault in the kernel. */
  if (copyout(base, &f, sizeof f) != 0)
    return -1;
  sync_icache(base + __builtin_offsetof(struct sigframe, tramp));

  r->x[0] = (uint64_t)info->signo;
  r->x[1] = base + __builtin_offsetof(struct sigframe, info);
  r->x[2] = base + __builtin_offsetof(struct sigframe, uc);
  r->x[30] = base + __builtin_offsetof(struct sigframe, tramp);
  r->sp = base;
  r->pc = handler;
  r->pstate &= PSTATE_NZCV;
  fpu_load_init(); /* the handler starts from a clean FP/SIMD state */
  return 0;
}

int arch_sigreturn(struct registers *r, uint32_t *blocked)
{
  /* The handler returned with sp back at the frame, then ran the trampoline. */
  uaddr_t base = r->sp;
  if (base < USER_BASE || base > USER_REGION_END - sizeof(struct sigframe))
    return -1;
  struct sigframe f;
  if (copyin(&f, base, sizeof f) != 0)
    return -1;

  *blocked = (uint32_t)f.saved_blocked;
  memcpy(r->x, f.uc.x, sizeof r->x);
  r->sp = f.uc.sp;
  r->pc = f.uc.pc;
  /* Only the condition flags come back: never another exception level or masked interrupts. */
  r->pstate = f.uc.pstate & PSTATE_NZCV;
  uint8_t fpbuf[FPU_AREA_BYTES + FPU_AREA_ALIGN - 1];
  uint8_t *fp = (uint8_t *)(((uintptr_t)fpbuf + FPU_AREA_ALIGN - 1) & ~(uintptr_t)(FPU_AREA_ALIGN - 1));
  memcpy(fp, f.fpu, sizeof f.fpu);
  fpu_sanitize(fp);
  fpu_restore(fp);
  return 0;
}

uaddr_t arch_crash_capture(struct crash_rec *c, const struct registers *r)
{
  c->vector = esr_class(r->esr);
  c->page_fault = esr_is_abort(r->esr);
  c->err = c->page_fault ? abort_err_bits(r->esr) : (unsigned)(r->esr & 0x1FFFFFF); /* else the ISS */
  c->addr = c->page_fault ? r->far : 0;
  c->pc = r->pc;
  c->sp = r->sp;
  const sys_word regs[CRASH_REGS] = {r->x[0], r->x[1], r->x[2], r->x[3], r->x[4], r->x[5], r->x[29], r->x[30]};
  for (int i = 0; i < CRASH_REGS; i++)
    c->regs[i] = regs[i];
  return r->x[29];
}

const char *arch_trap_name(unsigned ec)
{
  switch (ec) {
  case EC_UNKNOWN:
    return "undefined instruction";
  case EC_ILLEGAL_STATE:
    return "illegal execution state";
  case EC_SYSREG:
    return "trapped system register access";
  case EC_PC_ALIGN:
    return "pc alignment fault";
  case EC_SP_ALIGN:
    return "sp alignment fault";
  case EC_FP_EXC64:
    return "floating-point exception";
  case EC_BRK64:
    return "breakpoint (brk)";
  default:
    return "exception";
  }
}
