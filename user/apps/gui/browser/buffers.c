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

int browser_ensure_lines(int need)
{
  if (need <= B->line_cap)
    return 1;
  int nc = browser_grow_cap(B->line_cap, LINES_INIT, need);
  int *off = realloc(B->line_off, (unsigned)nc * sizeof *off);
  int *len = realloc(B->line_len, (unsigned)nc * sizeof *len);
  int *lx = realloc(B->line_x, (unsigned)nc * sizeof *lx);
  int *lw = realloc(B->line_w, (unsigned)nc * sizeof *lw);
  int *ly = realloc(B->line_y, (unsigned)nc * sizeof *ly);
  int *lh = realloc(B->line_h, (unsigned)nc * sizeof *lh);
  int *lp = realloc(B->line_px, (unsigned)nc * sizeof *lp);
  if (off)
    B->line_off = off;
  if (len)
    B->line_len = len;
  if (lx)
    B->line_x = lx;
  if (lw)
    B->line_w = lw;
  if (ly)
    B->line_y = ly;
  if (lh)
    B->line_h = lh;
  if (lp)
    B->line_px = lp;
  if (off && len && lx && lw && ly && lh && lp)
    B->line_cap = nc;
  return off && len && lx && lw && ly && lh && lp;
}

void browser_ov_add(int row, int off, int len, int x, int w, int h, int maxpx)
{
  if (B->n_fslices >= B->fslice_cap) {
    int nc = browser_grow_cap(B->fslice_cap, 256, B->n_fslices + 1);
    struct fslice *p = realloc(B->fslices, (unsigned)nc * sizeof *p);
    if (!p)
      return;
    B->fslices = p;
    B->fslice_cap = nc;
  }
  struct fslice *f = &B->fslices[B->n_fslices++];
  f->row = row;
  f->off = off;
  f->len = len;
  f->x = x;
  f->w = w;
  f->h = h;
  f->maxpx = maxpx;
}
