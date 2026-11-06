#include "syscall_window.h"
#include "idt.h"
#include "sched.h"
#include "timer.h"
#include "uaccess.h"
#include "uwin.h"
#include <stdint.h>

void syscall_window_create(struct registers *r)
{
  char title[64];
  if (r->edx && copyin_str(r->edx, title, sizeof title) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (!r->edx)
    title[0] = '\0';
  r->eax = (uint32_t)uwin_create((int)r->ebx, (int)r->ecx, title);
}

/* ecx = x | y << 16, edx = w | h << 16; both zero presents the whole buffer. */
void syscall_window_present(struct registers *r)
{
  r->eax = (uint32_t)uwin_present((int)r->ebx, (int)(r->ecx & 0xFFFF), (int)(r->ecx >> 16), (int)(r->edx & 0xFFFF),
                                  (int)(r->edx >> 16));
}

void syscall_window_poll(struct registers *r)
{
  struct win_event *ev = (struct win_event *)r->ecx;
  if (ev && !user_out_range(r->ecx, sizeof *ev)) {
    r->eax = (uint32_t)-1;
    return;
  }
  int got = ev ? uwin_poll((int)r->ebx, ev) : 0;
  if (!got && (int)r->edx) {
    __asm__ volatile("sti");
    timer_sleep(15);
  }
  r->eax = (uint32_t)got;
}

void syscall_window_close(struct registers *r) { r->eax = (uint32_t)uwin_close((int)r->ebx); }

void syscall_window_title(struct registers *r)
{
  char title[64];
  if (r->ecx && copyin_str(r->ecx, title, sizeof title) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  if (!r->ecx)
    title[0] = '\0';
  r->eax = (uint32_t)uwin_set_title((int)r->ebx, title);
}

void syscall_window_resizable(struct registers *r) { r->eax = (uint32_t)uwin_set_resizable((int)r->ebx, (int)r->ecx); }

void syscall_window_cursor(struct registers *r) { r->eax = (uint32_t)uwin_set_cursor((int)r->ebx, (int)r->ecx); }

void syscall_window_set_parent(struct registers *r)
{
  r->eax = (uint32_t)uwin_set_parent((int)r->ebx, (int)r->ecx, (int)r->edx);
}

void syscall_window_fb(struct registers *r) { r->eax = uwin_fb_vaddr((int)r->ebx); }

void syscall_window_owns(struct registers *r) { r->eax = (uint32_t)uwin_pid_has_window((uint32_t)r->ebx); }
