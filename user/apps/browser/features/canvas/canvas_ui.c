/* canvas_ui.c: publishes canvas pixels into the image pool for painting (UI thread). */
#include "canvas.h"
#include "main_int.h"

static void publish(struct canvas_rec *r)
{
  if (!r->dirty || !r->uid)
    return;
  r->dirty = 0;
  mutex_lock(&r->lock);
  unsigned long bytes = (unsigned long)r->c->w * r->c->h * 4;
  if (!r->front) {
    mutex_unlock(&r->lock);
    return;
  }
  for (int i = 0; i < B->pg->n_imgs; i++) {
    const struct inl_img *im = &B->pg->imgs[i];
    if (im->uid != r->uid || !im->ok || !im->is_argb || (unsigned long)im->clen != bytes || im->coff < 0 ||
        im->coff + im->clen > IMG_POOL)
      continue;
    memcpy(B->img_pool + im->coff, r->front, bytes);
    damage_lines(im->start_row, im->start_row + im->rows - 1);
  }
  mutex_unlock(&r->lock);
}

void canvas_tick(void) { canvas_each(publish); }
