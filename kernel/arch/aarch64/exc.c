/* exc.c: what every exception into EL1 does: interrupts, system calls, and faults (copy-on-write, a user signal,
 * or a crash report and kill; a kernel fault that isn't copy-on-write halts). */
#include "exc.h"
#include "arch.h"
#include "crash.h"
#include "irqflags.h"
#include "paging.h"
#include "regs.h"
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "timer_a64.h"

uint32_t abort_err_bits(uint64_t esr)
{
  unsigned ec = esr_class(esr);
  uint32_t fsc = (uint32_t)esr & 0x3F;
  uint32_t err = 0;
  if ((fsc & 0x3C) != 0x04) /* not a translation fault: the page is there */
    err |= 1;
  if ((ec == EC_DATA_ABORT_EL0 || ec == EC_DATA_ABORT_EL1) && (esr & (1u << 6))) /* WnR */
    err |= 2;
  if (ec == EC_DATA_ABORT_EL0 || ec == EC_INSN_ABORT_EL0)
    err |= 4;
  if (ec == EC_INSN_ABORT_EL0 || ec == EC_INSN_ABORT_EL1)
    err |= 16;
  return err;
}

/* What a handler sees of an EL0 fault (signal.h): the cause, and the address it concerns. */
static struct k_siginfo fault_siginfo(const struct registers *r)
{
  unsigned ec = esr_class(r->esr);
  struct k_siginfo info = {SIGILL, ILL_ILLOPC, r->pc, ec, (uint32_t)(r->esr & 0x1FFFFFF)};
  switch (ec) {
  case EC_INSN_ABORT_EL0:
  case EC_DATA_ABORT_EL0:
    info.signo = SIGSEGV;
    info.err = abort_err_bits(r->esr);
    info.code = info.err & 1 ? SEGV_ACCERR : SEGV_MAPERR;
    info.addr = r->far;
    break;
  case EC_PC_ALIGN:
  case EC_SP_ALIGN:
    info.signo = SIGBUS;
    info.code = 0;
    info.addr = ec == EC_PC_ALIGN ? r->far : r->sp;
    break;
  case EC_FP_EXC64:
    info.signo = SIGFPE;
    info.code = 0;
    break;
  case EC_BRK64:
    info.signo = SIGTRAP;
    info.code = 0;
    break;
  default: /* undefined instruction, illegal execution state, a trapped system register */
    break;
  }
  return info;
}

static void halt_with(const struct registers *r, uint64_t kind, const char *what)
{
  uart_puts("\n*** CPU EXCEPTION: ");
  uart_puts(what);
  uart_puts(" ***\n  kind=");
  uart_puthex(kind);
  uart_puts(" esr=");
  uart_puthex(r->esr);
  uart_puts(" far=");
  uart_puthex(r->far);
  uart_puts("\n  pc=");
  uart_puthex(r->pc);
  uart_puts(" sp_el0=");
  uart_puthex(r->sp);
  uart_puts(" lr=");
  uart_puthex(r->x[30]);
  uart_puts("\n  system halted.\n");
  for (;;)
    cpu_halt();
}

static void irq(struct registers *r)
{
  uint32_t iar = gic_ack();
  uint32_t id = iar & 0x3FF;
  if (id >= 1020) /* spurious */
    return;
  sched_kernel_enter();
  int tick = id == IRQ_TIMER_PPI;
  if (tick)
    timer_rearm(); /* the timer interrupt is level-triggered: lower it before the EOI */
  /* EOI before the handler: the tick may switch tasks and only come back here much later. */
  gic_eoi(iar);
  if (tick)
    timer_tick();
  /* A CPU-bound task that makes no system calls still gets its signals at the next tick. */
  sched_signal_deliver(r);
  sched_kernel_leave();
}

static void user_sync(struct registers *r)
{
  sched_kernel_enter();
  if (esr_class(r->esr) == EC_SVC64) {
    r->sa[0] = r->x[0];
    r->sa[1] = r->x[1];
    r->sa[2] = r->x[2];
    syscall_dispatch(r);
    sched_signal_deliver(r);
    sched_kernel_leave();
    return;
  }
  if (esr_is_abort(r->esr) && paging_handle_page_fault(r->far, abort_err_bits(r->esr))) {
    sched_kernel_leave();
    return;
  }
  struct k_siginfo info = fault_siginfo(r);
  if (sched_fault_signal(r, &info)) {
    sched_kernel_leave();
    return;
  }
  crash_record(sched_current_name(), r);
  sched_exit(139);
}

/* kind = vector index: 0-3 current EL SP0, 4-7 current EL SPx, 8-11 lower EL A64, 12-15 lower EL A32; within each
 * group 0 sync, 1 irq, 2 fiq, 3 serror. */
void arch_exception(struct registers *r, uint64_t kind)
{
  uint64_t type = kind & 3;
  if (type == 1) {
    irq(r);
    return;
  }
  if (type == 0 && kind >= 8 && kind < 12) {
    user_sync(r);
    return;
  }
  /* The kernel writing a copy-on-write page (copyout into a forked task's memory). */
  if (type == 0 && kind >= 4 && kind < 8 && esr_class(r->esr) == EC_DATA_ABORT_EL1 &&
      paging_handle_page_fault(r->far, abort_err_bits(r->esr)))
    return;
  halt_with(r, kind, type == 3 ? "SError" : kind >= 12 ? "AArch32 EL0" : "kernel fault");
}
