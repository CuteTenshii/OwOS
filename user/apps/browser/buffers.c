#include "browser.h"

int browser_grow_cap(int cap, int init, int need)
{
  int nc = cap > 0 ? cap : init;
  while (nc < need)
    nc += nc / 2 + 1;
  return nc;
}

int browser_ensure_raw(int need)
{
  if (need <= B->pg->raw_cap)
    return 1;
  int nc = browser_grow_cap(B->pg->raw_cap, RAW_INIT, need);
  browser_buf_lock();
  char *p = realloc(B->pg->raw, (unsigned)nc);
  if (p) {
    B->pg->raw = p;
    B->pg->raw_cap = nc;
  }
  browser_buf_unlock();
  return p != 0;
}

int browser_ensure_text(int need)
{
  if (need <= B->pg->text_cap)
    return 1;
  int nc = browser_grow_cap(B->pg->text_cap, TEXT_INIT, need);
  browser_buf_lock();
  char *t = realloc(B->pg->text, (unsigned)nc);
  unsigned char *a = realloc(B->pg->attr, (unsigned)nc);
  unsigned char *p = realloc(B->pg->bp, (unsigned)nc);
  unsigned char *y = realloc(B->pg->align, (unsigned)nc);
  unsigned char *z = realloc(B->pg->fsz, (unsigned)nc);
  unsigned char *l = realloc(B->pg->lh, (unsigned)nc);
  unsigned *f = realloc(B->pg->fg, (unsigned)nc * sizeof *f);
  unsigned *g = realloc(B->pg->bg, (unsigned)nc * sizeof *g);
  if (t)
    B->pg->text = t;
  if (a)
    B->pg->attr = a;
  if (p)
    B->pg->bp = p;
  if (y)
    B->pg->align = y;
  if (z)
    B->pg->fsz = z;
  if (l)
    B->pg->lh = l;
  if (f)
    B->pg->fg = f;
  if (g)
    B->pg->bg = g;
  if (t && a && p && y && z && l && f && g)
    B->pg->text_cap = nc;
  browser_buf_unlock();
  return t && a && p && y && z && l && f && g;
}

int browser_ensure_links(int need)
{
  if (need <= B->pg->links_cap)
    return 1;
  int nc = browser_grow_cap(B->pg->links_cap, LINKS_INIT, need);
  browser_buf_lock();
  struct link *p = realloc(B->pg->links, (unsigned)nc * sizeof *p);
  if (p) {
    B->pg->links = p;
    B->pg->links_cap = nc;
  }
  browser_buf_unlock();
  return p != 0;
}

int browser_ensure_regs(int need)
{
  if (need <= B->pg->regs_cap)
    return 1;
  int nc = browser_grow_cap(B->pg->regs_cap, LINKS_INIT, need);
  browser_buf_lock();
  struct pregion *p = realloc(B->pg->regs, (unsigned)nc * sizeof *p);
  if (p) {
    B->pg->regs = p;
    B->pg->regs_cap = nc;
  }
  browser_buf_unlock();
  return p != 0;
}

int browser_ensure_imgs(int need)
{
  if (need <= B->pg->imgs_cap)
    return 1;
  int nc = browser_grow_cap(B->pg->imgs_cap, IMGS_INIT, need);
  browser_buf_lock();
  struct inl_img *p = realloc(B->pg->imgs, (unsigned)nc * sizeof *p);
  if (p) {
    B->pg->imgs = p;
    B->pg->imgs_cap = nc;
  }
  browser_buf_unlock();
  return p != 0;
}
