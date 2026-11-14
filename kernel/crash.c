/* crash.c: crash reporter (see crash.h).
 * A user fault never panics: crash_record() captures registers and a backtrace, dumps to the serial console and the
 * process is killed. The compositor drains reports via crash_poll (SYS_CRASH_POLL). */
#include "crash.h"
#include "loader.h"
#include "paging.h"
#include "sched.h"
#include "serial.h"
#include "task_arch.h"
#include <stdint.h>

#define MAXCRASH 4

static struct crash_rec reports[MAXCRASH];
static int report_state[MAXCRASH]; /* 0 free, 1 pending (the compositor has not taken it yet) */

/* `v` in hex, at least 8 digits and all of a wider one (no "0x"); returns out for chaining. */
static const char *hexw(uint64_t v, char *out)
{
  static const char d[] = "0123456789abcdef";
  int n = 8;
  while (n < 16 && (v >> (n * 4)))
    n++;
  for (int i = 0; i < n; i++)
    out[i] = d[(v >> ((n - 1 - i) * 4)) & 0xF];
  out[n] = '\0';
  return out;
}

/* Plain-English page-fault cause from the error code and faulting address. */
static const char *pf_cause(uint32_t err, uaddr_t addr, char *buf)
{
  int i = 0;
  const char *parts[4];
  parts[0] = (err & 4) ? "user-mode " : "kernel ";
  parts[1] = (err & 16) ? "instruction fetch from " : (err & 2) ? "write to " : "read of ";
  parts[2] = (err & 1) ? "a protected page" : "unmapped memory";
  parts[3] = addr < 0x1000 ? " (NULL dereference)"
             : (addr >= USER_BASE && addr < USER_REGION_END)
                 ? " (bad heap/stack pointer - likely corruption or use-after-free)"
                 : "";
  for (int p = 0; p < 4; p++)
    for (const char *s = parts[p]; *s && i < 126; s++)
      buf[i++] = *s;
  buf[i] = '\0';
  return buf;
}

/* Per-app function symbol table (user/apps/build.mk): ">app" sections of "<hexaddr> name" lines sorted by address. */
extern const char _binary_appsyms_start[], _binary_appsyms_end[];

/* Resolve `addr` to "func+0xNN" within `app`'s symbol section (largest symbol
 * address <= addr). Returns 1 and fills out on success, 0 if unknown. */
static bool crash_symbolize(const char *app, uaddr_t addr, char *out, int cap)
{
  const char *p = _binary_appsyms_start, *end = _binary_appsyms_end;
  int in = 0;
  uaddr_t best = 0;
  const char *bn = 0;
  int bl = 0;
  out[0] = '\0';
  if (!app || !app[0])
    return false;
  while (p < end) {
    if (*p == '>') { /* section header ">app" */
      const char *s = ++p;
      while (p < end && *p != '\n')
        p++;
      int L = (int)(p - s), i = 0;
      while (i < L && app[i] && app[i] == s[i])
        i++;
      int match = (i == L && !app[i]);
      if (in && !match)
        break;
      in = match;
    } else if (in) { /* "<hexaddr> name" */
      uaddr_t a = 0;
      while (p < end && *p != ' ' && *p != '\n') {
        int c = *p, d = (c >= '0' && c <= '9')   ? c - '0'
                        : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                        : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                 : -1;
        if (d < 0)
          break;
        a = a * 16 + (uaddr_t)d;
        p++;
      }
      if (p < end && *p == ' ') {
        const char *ns = ++p;
        while (p < end && *p != '\n')
          p++;
        if (a <= addr && a >= best) {
          best = a;
          bn = ns;
          bl = (int)(p - ns);
        }
      }
    }
    while (p < end && *p != '\n')
      p++;
    if (p < end)
      p++;
  }
  if (!bn)
    return false;
  int o = 0;
  for (int i = 0; i < bl && o < cap - 20; i++)
    out[o++] = bn[i];
  uint64_t off = addr - best;
  out[o++] = '+';
  out[o++] = '0';
  out[o++] = 'x';
  {
    static const char hd[] = "0123456789abcdef";
    bool st = false;
    for (int sh = 60; sh >= 0; sh -= 4) {
      int nb = (int)((off >> sh) & 0xF);
      if (nb || st || sh == 0) {
        out[o++] = hd[nb];
        st = true;
      }
    }
  }
  out[o] = '\0';
  return true;
}

/* Emit the full report to the serial console as text (serial_* is heapless and lockless, safe from the faulting
 * task). */
static void crash_dump_serial(const struct crash_rec *c)
{
  static const char *const reg_names[CRASH_REGS] = CRASH_REG_NAMES;
  char hx[20], cb[128];
  serial_write("\n=== CRASH: ");
  serial_write(c->name[0] ? c->name : "?");
  const char *thread = sched_current_thread_name(); /* dumped from the faulting task, so this is its thread */
  if (thread[0]) {
    serial_write(" [");
    serial_write(thread);
    serial_write("]");
  }
  serial_write(" - ");
  serial_write(c->page_fault ? "page fault" : arch_trap_name(c->vector));
  serial_write(" ===\n");
  if (c->page_fault) {
    serial_write("cause: ");
    serial_write(pf_cause(c->err, c->addr, cb));
    serial_write("\n");
  }
  serial_write("pc=");
  serial_write(hexw(c->pc, hx));
  if (c->page_fault) {
    serial_write(" addr=");
    serial_write(hexw(c->addr, hx));
  }
  serial_write(" err=");
  serial_write(hexw(c->err, hx));
  serial_write(" sp=");
  serial_write(hexw(c->sp, hx));
  for (int i = 0; i < CRASH_REGS; i++) {
    serial_write(i % 4 ? " " : "\n");
    serial_write(reg_names[i]);
    serial_write("=");
    serial_write(hexw(c->regs[i], hx));
  }
  serial_write("\ntrace (addr2line -e user/apps/");
  serial_write(c->name[0] ? c->name : "app");
  serial_write("/");
  serial_write(c->name[0] ? c->name : "app");
  serial_write(".elf):\n");
  for (int i = 0; i < c->ntrace; i++) {
    char sym[96];
    serial_write("  ");
    serial_write(hexw(c->trace[i], hx));
    if (crash_symbolize(c->name, c->trace[i], sym, sizeof sym)) {
      serial_write("  ");
      serial_write(sym);
    }
    serial_write(i == 0 ? "  <- fault\n" : "\n");
  }
}

int crash_symbolize_self(uaddr_t addr, char *out, int cap)
{
  return cap >= 24 && crash_symbolize(sched_current_name(), addr, out, cap);
}

void crash_record(const char *name, struct registers *r)
{
  int slot = -1;
  for (int i = 0; i < MAXCRASH; i++)
    if (!report_state[i]) {
      slot = i;
      break;
    }
  if (slot < 0)
    return;
  struct crash_rec *c = &reports[slot];
  *c = (struct crash_rec){0};

  int i = 0;
  for (; name && name[i] && i < (int)sizeof c->name - 1; i++)
    c->name[i] = name[i];
  c->name[i] = '\0';

  /* Backtrace via the frame records ([saved fp][return address], a user word each); every read is probed so a
   * corrupt frame pointer can't fault. */
  uaddr_t fp = arch_crash_capture(c, r);
  c->ntrace = 0;
  c->trace[c->ntrace++] = c->pc;
  const uaddr_t word = sizeof(uintptr_t);
  while (c->ntrace < CRASH_TRACE_MAX && fp >= USER_BASE && fp <= USER_REGION_END - 2 * word && paging_user_mapped(fp) &&
         paging_user_mapped(fp + word)) {
    c->trace[c->ntrace++] = *(volatile uintptr_t *)(uintptr_t)(fp + word);
    uaddr_t next = *(volatile uintptr_t *)(uintptr_t)fp;
    if (next <= fp)
      break; /* frames must grow walking up */
    fp = next;
  }
  report_state[slot] = 1;
  crash_dump_serial(c);
}

/* SYS_CRASH_POLL: hand pending reports to the compositor, consuming each. */
int crash_poll(struct crash_rec *out, int max)
{
  int n = 0;
  for (int i = 0; i < MAXCRASH && n < max; i++) {
    if (report_state[i] != 1)
      continue;
    out[n++] = reports[i];
    report_state[i] = 0;
  }
  return n;
}
