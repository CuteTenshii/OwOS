#include "syscall_graphics.h"
#include "console.h"
#include "crash.h"
#include "fb.h"
#include "keyboard.h"
#include "loader.h"
#include "mouse.h"
#include "paging.h"
#include "regs.h"
#include "sched.h"
#include "uaccess.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The task that mapped the back buffer (the compositor); the kernel console stops reading the keyboard while it
 * lives. */
static uint32_t fb_owner;

bool graphics_fb_owned(void) { return fb_owner != 0; }

void graphics_task_exiting(uint32_t id)
{
  if (id == fb_owner)
    fb_owner = 0;
}

void syscall_graphics_fb_info(struct registers *r)
{
  struct fbinfo {
    int w, h, pitch, bpp;
  } *out;
  if (sched_uid() != 0 || !user_out_range(SC_ARG0(r), sizeof *out)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  out = (struct fbinfo *)SC_ARG0(r);
  fb_poll_resize();
  fb_dims(&out->w, &out->h, &out->pitch, &out->bpp);
  SC_RET(r) = 0;
}

void syscall_graphics_fb_map(struct registers *r)
{
  if (sched_uid() != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  uint32_t phys = fb_backbuffer_phys(), bytes = fb_backbuffer_bytes();
  if (!phys || !bytes) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  int npages = (int)((bytes + 4095u) / 4096u);
  if (paging_map_user_at(sched_current_dir(), COMPOSITOR_FB_ADDR, phys, npages) < 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (!fb_owner)
    fb_owner = sched_current_id();
  con_use_framebuffer(1); /* the back buffer is the compositor's now: the verbose boot console must stop drawing */
  SC_RET(r) = COMPOSITOR_FB_ADDR;
}

/* ebx = struct fb_rect array, ecx = count (0 means one). */
void syscall_graphics_fb_present(struct registers *r)
{
  enum { MAX_RECTS = 32 };
  uint32_t n = (uint32_t)SC_ARG1(r) ? (uint32_t)SC_ARG1(r) : 1;
  if (sched_uid() != 0 || n > MAX_RECTS || !user_out_range(SC_ARG0(r), n * sizeof(struct fb_rect))) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  fb_present_rects((const struct fb_rect *)SC_ARG0(r), (int)n);
  SC_RET(r) = 0;
}

/* ebx = ARGB bitmap or 0 to hide, ecx = w | h << 8 | hot_x << 16 | hot_y << 24, edx = ui scale. */
void syscall_graphics_fb_cursor(struct registers *r)
{
  enum { MAX_DIM = 64 };
  static uint32_t pix[MAX_DIM * MAX_DIM];
  uint32_t shape = (uint32_t)SC_ARG1(r); /* w | h << 8 | hot_x << 16 | hot_y << 24 */
  int w = shape & 0xff, h = (shape >> 8) & 0xff;
  int hx = (shape >> 16) & 0xff, hy = (shape >> 24) & 0xff;
  if (sched_uid() != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  if (!SC_ARG0(r)) {
    SC_RET(r) = (uint32_t)fb_cursor_set(NULL, 0, 0, 0, 0, 1);
    return;
  }
  if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM || hx >= w || hy >= h ||
      copyin(pix, SC_ARG0(r), (uint32_t)(w * h) * 4) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  int rc = fb_cursor_set(pix, w, h, hx, hy, (int)SC_ARG2(r));
  if (rc == 0)
    fb_cursor_move(mouse_x(), mouse_y());
  SC_RET(r) = (uint32_t)rc;
}

void syscall_graphics_input_poll(struct registers *r)
{
  enum { INPUT_KEYS = 32 };
  struct inputstate {
    int mx, my, buttons, wheel;
    int nkeys;
    struct {
      int c, mods;
    } keys[INPUT_KEYS];
  } *out;
  if (sched_uid() != 0 || !user_out_range(SC_ARG0(r), sizeof *out)) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  out = (struct inputstate *)SC_ARG0(r);
  out->mx = mouse_x();
  out->my = mouse_y();
  out->buttons = mouse_buttons();
  out->wheel = mouse_wheel();
  int n = 0;
  char c;
  unsigned char mods;
  while (n < INPUT_KEYS && keyboard_poll(&c, &mods)) {
    out->keys[n].c = (unsigned char)c;
    out->keys[n].mods = (int)mods;
    n++;
  }
  if (n < INPUT_KEYS && keyboard_take_interrupt()) {
    out->keys[n].c = KEY_INTR;
    out->keys[n].mods = 0;
    n++;
  }
  if (n < INPUT_KEYS && keyboard_take_screenshot()) {
    out->keys[n].c = KEY_PRTSC;
    out->keys[n].mods = 0;
    n++;
  }
  out->nkeys = n;
  SC_RET(r) = 0;
}

void syscall_graphics_input_bounds(struct registers *r)
{
  if (sched_uid() != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  mouse_set_bounds((int)SC_ARG0(r), (int)SC_ARG1(r));
  SC_RET(r) = 0;
}

void syscall_graphics_symbolize(struct registers *r)
{
  char sym[96];
  int cap = (int)SC_ARG2(r) > (int)sizeof sym ? (int)sizeof sym : (int)SC_ARG2(r);
  if (cap <= 0 || !user_out_range(SC_ARG1(r), (uint32_t)cap) || !crash_symbolize_self((uint32_t)SC_ARG0(r), sym, cap) ||
      copyout(SC_ARG1(r), sym, (uint32_t)cap) != 0) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = 0;
}

void syscall_graphics_crash_poll(struct registers *r)
{
  int max = (int)SC_ARG1(r);
  if (max > 4096)
    max = 4096;
  if (sched_uid() != 0 || max <= 0 || !user_out_range(SC_ARG0(r), (uint32_t)max * sizeof(struct crash_rec))) {
    SC_RET(r) = (uint32_t)-1;
    return;
  }
  SC_RET(r) = (uint32_t)crash_poll((struct crash_rec *)(uintptr_t)SC_ARG0(r), max);
}
