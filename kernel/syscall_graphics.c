#include "syscall_graphics.h"
#include "crash.h"
#include "fb.h"
#include "idt.h"
#include "keyboard.h"
#include "loader.h"
#include "mouse.h"
#include "paging.h"
#include "sched.h"
#include "uaccess.h"
#include "uwin.h"
#include <stdint.h>

void syscall_graphics_fb_info(struct registers *r)
{
  struct fbinfo {
    int w, h, pitch, bpp;
  } *out;
  if (sched_uid() != 0 || !user_out_range(r->ebx, sizeof *out)) {
    r->eax = (uint32_t)-1;
    return;
  }
  out = (struct fbinfo *)r->ebx;
  fb_dims(&out->w, &out->h, &out->pitch, &out->bpp);
  r->eax = 0;
}

void syscall_graphics_fb_map(struct registers *r)
{
  if (sched_uid() != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint32_t phys = fb_backbuffer_phys(), bytes = fb_backbuffer_bytes();
  if (!phys || !bytes) {
    r->eax = (uint32_t)-1;
    return;
  }
  int npages = (int)((bytes + 4095u) / 4096u);
  if (paging_map_user_at(sched_current_dir(), COMPOSITOR_FB_VADDR, phys, npages) < 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  uwin_compositor_register(sched_current_id());
  r->eax = COMPOSITOR_FB_VADDR;
}

void syscall_graphics_fb_present(struct registers *r)
{
  struct fbrect {
    int x, y, w, h;
  } *rect;
  if (sched_uid() != 0 || !user_out_range(r->ebx, sizeof *rect)) {
    r->eax = (uint32_t)-1;
    return;
  }
  rect = (struct fbrect *)r->ebx;
  fb_present_rect(rect->x, rect->y, rect->w, rect->h);
  r->eax = 0;
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
  if (sched_uid() != 0 || !user_out_range(r->ebx, sizeof *out)) {
    r->eax = (uint32_t)-1;
    return;
  }
  out = (struct inputstate *)r->ebx;
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
  r->eax = 0;
}

void syscall_graphics_input_bounds(struct registers *r)
{
  if (sched_uid() != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  mouse_set_bounds((int)r->ebx, (int)r->ecx);
  r->eax = 0;
}

void syscall_graphics_win_enum(struct registers *r)
{
  int max = (int)r->ecx;
  if (max > 4096)
    max = 4096;
  if (sched_uid() != 0 || max <= 0 || !user_out_range(r->ebx, (uint32_t)max * sizeof(struct winrec))) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)uwin_enum((struct winrec *)r->ebx, max);
}

void syscall_graphics_win_map(struct registers *r)
{
  struct winmap {
    uint32_t vaddr;
    int w, h, stride;
  } *out;
  if (sched_uid() != 0 || !user_out_range(r->ecx, sizeof *out)) {
    r->eax = (uint32_t)-1;
    return;
  }
  int id = (int)r->ebx;
  if (id < 0 || (uint32_t)id >= WIN_MAP_MAX) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint32_t at = WIN_MAP_BASE + (uint32_t)id * WIN_MAP_SLOT_BYTES;
  int w = 0, h = 0, stride = 0;
  if (uwin_map(id, at, &w, &h, &stride) < 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  out = (struct winmap *)r->ecx;
  out->vaddr = at;
  out->w = w;
  out->h = h;
  out->stride = stride;
  r->eax = 0;
}

void syscall_graphics_win_unmap(struct registers *r)
{
  if (sched_uid() != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  int id = (int)r->ebx;
  if (id < 0 || (uint32_t)id >= WIN_MAP_MAX) {
    r->eax = (uint32_t)-1;
    return;
  }
  uint32_t at = WIN_MAP_BASE + (uint32_t)id * WIN_MAP_SLOT_BYTES;
  r->eax = (uint32_t)uwin_unmap(id, sched_current_dir(), at);
}

void syscall_graphics_win_geom(struct registers *r)
{
  struct wg {
    int x, y, w, h;
  } geom;
  if (sched_uid() != 0 || !user_range(r->ecx, sizeof geom) || copyin(&geom, r->ecx, sizeof geom) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)uwin_set_geom((int)r->ebx, geom.x, geom.y, geom.w, geom.h);
}

void syscall_graphics_win_deliver(struct registers *r)
{
  if (sched_uid() != 0 || !user_range(r->ecx, sizeof(struct winevent))) {
    r->eax = (uint32_t)-1;
    return;
  }
  struct winevent ev;
  if (copyin(&ev, r->ecx, sizeof ev) != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)uwin_deliver((int)r->ebx, &ev);
}

void syscall_graphics_win_key_space(struct registers *r)
{
  if (sched_uid() != 0) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)uwin_key_space((int)r->ebx);
}

void syscall_graphics_crash_poll(struct registers *r)
{
  int max = (int)r->ecx;
  if (max > 4096)
    max = 4096;
  if (sched_uid() != 0 || max <= 0 || !user_out_range(r->ebx, (uint32_t)max * sizeof(struct crash_rec))) {
    r->eax = (uint32_t)-1;
    return;
  }
  r->eax = (uint32_t)crash_poll((struct crash_rec *)r->ebx, max);
}
