/* crash.c: crash reporter (see crash.h).
 * A user fault never panics: crash_record() captures registers and a backtrace, dumps to COM1 and the
 * process is killed. The compositor drains reports via crash_poll (SYS_CRASH_POLL). */
#include "crash.h"
#include "creg.h"
#include "idt.h"
#include "loader.h"
#include "paging.h"
#include "serial.h"
#include <stdint.h>

#define MAXCRASH 4
#define CRASH_TRACE CRASH_TRACE_MAX

struct crash_info {
  int state; /* 0 free, 1 pending (needs window), 2 shown   */
  char name[24];
  uint32_t vector, err, eip, cr2;
  uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp;
  uint32_t trace[CRASH_TRACE];
  int ntrace;
};

static struct crash_info reports[MAXCRASH];

/* Format `v` as 8-digit hex into out (no "0x"); returns out for chaining. */
static const char *hex8(uint32_t v, char *out)
{
  static const char d[] = "0123456789abcdef";
  for (int i = 0; i < 8; i++)
    out[i] = d[(v >> ((7 - i) * 4)) & 0xF];
  out[8] = '\0';
  return out;
}

static const char *vec_name(uint32_t v)
{
  return v == 14   ? "page fault"
         : v == 13 ? "general protection fault"
         : v == 6  ? "invalid opcode"
         : v == 0  ? "divide error"
         : v == 1  ? "debug"
         : v == 3  ? "breakpoint"
         : v == 8  ? "double fault"
         : v == 12 ? "stack fault"
                   : "exception";
}

/* Plain-English page-fault cause from the error code and faulting address. */
static const char *pf_cause(uint32_t vec, uint32_t err, uint32_t cr2, char *buf)
{
  int i = 0;
  const char *parts[4];
  buf[0] = '\0';
  if (vec != 14)
    return buf;
  parts[0] = (err & 4) ? "user-mode " : "kernel ";
  parts[1] = (err & 16) ? "instruction fetch from " : (err & 2) ? "write to " : "read of ";
  parts[2] = (err & 1) ? "a protected page" : "unmapped memory";
  parts[3] = cr2 < 0x1000 ? " (NULL dereference)"
             : (cr2 >= USER_BASE && cr2 < USER_REGION_END)
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
static bool crash_symbolize(const char *app, uint32_t addr, char *out, int cap)
{
  const char *p = _binary_appsyms_start, *end = _binary_appsyms_end;
  int in = 0;
  uint32_t best = 0;
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
      uint32_t a = 0;
      while (p < end && *p != ' ' && *p != '\n') {
        int c = *p, d = (c >= '0' && c <= '9')   ? c - '0'
                        : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                        : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                 : -1;
        if (d < 0)
          break;
        a = a * 16 + (uint32_t)d;
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
  for (int i = 0; i < bl && o < cap - 12; i++)
    out[o++] = bn[i];
  uint32_t off = addr - best;
  out[o++] = '+';
  out[o++] = '0';
  out[o++] = 'x';
  {
    static const char hd[] = "0123456789abcdef";
    bool st = false;
    for (int sh = 28; sh >= 0; sh -= 4) {
      int nb = (off >> sh) & 0xF;
      if (nb || st || sh == 0) {
        out[o++] = hd[nb];
        st = true;
      }
    }
  }
  out[o] = '\0';
  return true;
}

/* Emit the full report to COM1 as text (serial_* is heapless and lockless, safe from the faulting task). */
static void crash_dump_serial(const struct crash_info *c)
{
  char hx[12], cb[128];
  serial_write("\n=== CRASH: ");
  serial_write(c->name[0] ? c->name : "?");
  serial_write(" - ");
  serial_write(vec_name(c->vector));
  serial_write(" ===\n");
  if (c->vector == 14) {
    serial_write("cause: ");
    serial_write(pf_cause(c->vector, c->err, c->cr2, cb));
    serial_write("\n");
  }
  serial_write("eip=");
  serial_write(hex8(c->eip, hx));
  if (c->vector == 14) {
    serial_write(" cr2=");
    serial_write(hex8(c->cr2, hx));
  }
  serial_write(" err=");
  serial_write(hex8(c->err, hx));
  serial_write("\n");
  serial_write("eax=");
  serial_write(hex8(c->eax, hx));
  serial_write(" ebx=");
  serial_write(hex8(c->ebx, hx));
  serial_write(" ecx=");
  serial_write(hex8(c->ecx, hx));
  serial_write(" edx=");
  serial_write(hex8(c->edx, hx));
  serial_write("\n");
  serial_write("esi=");
  serial_write(hex8(c->esi, hx));
  serial_write(" edi=");
  serial_write(hex8(c->edi, hx));
  serial_write(" ebp=");
  serial_write(hex8(c->ebp, hx));
  serial_write(" esp=");
  serial_write(hex8(c->esp, hx));
  serial_write("\n");
  serial_write("trace (addr2line -e user/apps/");
  serial_write(c->name[0] ? c->name : "app");
  serial_write("/");
  serial_write(c->name[0] ? c->name : "app");
  serial_write(".elf):\n");
  for (int i = 0; i < c->ntrace; i++) {
    char sym[96];
    serial_write("  ");
    serial_write(hex8(c->trace[i], hx));
    if (crash_symbolize(c->name, c->trace[i], sym, sizeof sym)) {
      serial_write("  ");
      serial_write(sym);
    }
    serial_write(i == 0 ? "  <- fault\n" : "\n");
  }
}

void crash_record(const char *name, struct registers *r)
{
  int slot = -1;
  for (int i = 0; i < MAXCRASH; i++)
    if (!reports[i].state) {
      slot = i;
      break;
    }
  if (slot < 0)
    return;
  struct crash_info *c = &reports[slot];

  int i = 0;
  for (; name && name[i] && i < (int)sizeof c->name - 1; i++)
    c->name[i] = name[i];
  c->name[i] = '\0';

  c->vector = r->int_no;
  c->err = r->err_code;
  c->eip = r->eip;
  c->cr2 = 0;
  if (r->int_no == 14)
    c->cr2 = (uint32_t)read_cr2();
  c->eax = r->eax;
  c->ebx = r->ebx;
  c->ecx = r->ecx;
  c->edx = r->edx;
  c->esi = r->esi;
  c->edi = r->edi;
  c->ebp = r->ebp;
  c->esp = r->esp;

  /* Backtrace via saved return addresses; every read is probed so a corrupt frame pointer can't fault. */
  c->ntrace = 0;
  c->trace[c->ntrace++] = r->eip;
  uint32_t bp = r->ebp;
  while (c->ntrace < CRASH_TRACE && bp >= USER_BASE && bp + 8 <= USER_REGION_END && paging_user_mapped(bp) &&
         paging_user_mapped(bp + 4)) {
    uint32_t ret = *(volatile uint32_t *)(uintptr_t)(bp + 4);
    c->trace[c->ntrace++] = ret;
    uint32_t nbp = *(volatile uint32_t *)(uintptr_t)bp;
    if (nbp <= bp)
      break; /* frames must grow walking up           */
    bp = nbp;
  }
  c->state = 1;
  crash_dump_serial(c);
}

/* SYS_CRASH_POLL: hand pending reports to the compositor, consuming each (state 1 to free). */
int crash_poll(struct crash_rec *out, int max)
{
  int n = 0;
  for (int i = 0; i < MAXCRASH && n < max; i++) {
    if (reports[i].state != 1)
      continue;
    const struct crash_info *c = &reports[i];
    struct crash_rec *o = &out[n++];
    for (int k = 0; k < (int)sizeof o->name; k++)
      o->name[k] = c->name[k];
    o->vector = c->vector;
    o->err = c->err;
    o->eip = c->eip;
    o->cr2 = c->cr2;
    o->eax = c->eax;
    o->ebx = c->ebx;
    o->ecx = c->ecx;
    o->edx = c->edx;
    o->esi = c->esi;
    o->edi = c->edi;
    o->ebp = c->ebp;
    o->esp = c->esp;
    o->ntrace = c->ntrace;
    for (int k = 0; k < CRASH_TRACE_MAX; k++)
      o->trace[k] = c->trace[k];
    reports[i].state = 0;
  }
  return n;
}
