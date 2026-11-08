/* animimg.c - see animimg.h. Players are keyed by the image uid, which survives layout walks and pool carry-over. */
#include "animimg.h"
#include "main_int.h"
#include "mutex.h"

#define ANIMIMG_MAX 16
#define ONSCREEN_MS 300 /* an image not drawn for this long is paused */
#define ORPHAN_MS 1500  /* its uid gone from the page for this long: the page moved on */

struct player {
  int uid;
  struct imganim *a;
  unsigned char *data;
  unsigned next_ms, seen_ms, orphan_ms;
  bool orphaned;
};

static struct player g_players[ANIMIMG_MAX];
static int g_nplayers;
static mutex_t g_lock = MUTEX_INIT;

static void player_free(int i)
{
  imganim_close(g_players[i].a);
  free(g_players[i].data);
  g_players[i] = g_players[--g_nplayers];
}

bool animimg_add(int uid, struct imganim *a, unsigned char *data, unsigned delay_ms)
{
  bool ok = false;
  mutex_lock(&g_lock);
  if (g_nplayers < ANIMIMG_MAX) {
    unsigned now = msec();
    g_players[g_nplayers++] = (struct player){uid, a, data, now + delay_ms, now, 0, false};
    ok = true;
  }
  mutex_unlock(&g_lock);
  return ok;
}

void animimg_seen(int uid)
{
  mutex_lock(&g_lock);
  for (int i = 0; i < g_nplayers; i++)
    if (g_players[i].uid == uid)
      g_players[i].seen_ms = msec();
  mutex_unlock(&g_lock);
}

/* Copy the canvas into every pool slot that shows `uid` and repaint just those rows. */
static void publish(const struct player *p)
{
  unsigned long bytes = (unsigned long)p->a->w * p->a->h * 4;
  for (int i = 0; i < B->pg->n_imgs; i++) {
    const struct inl_img *im = &B->pg->imgs[i];
    if (im->uid != p->uid)
      continue;
    if (im->ok && im->is_argb && (unsigned long)im->clen == bytes && im->coff >= 0 && im->coff + im->clen <= IMG_POOL) {
      memcpy(B->img_pool + im->coff, p->a->rgba, bytes);
      damage_lines(im->start_row, im->start_row + im->rows - 1);
    }
  }
}

static bool on_page(int uid)
{
  for (int i = 0; i < B->pg->n_imgs; i++)
    if (B->pg->imgs[i].uid == uid)
      return true;
  return false;
}

void animimg_tick(void)
{
  unsigned now = msec();
  mutex_lock(&g_lock);
  for (int i = 0; i < g_nplayers;) {
    struct player *p = &g_players[i];
    if (!on_page(p->uid)) {
      if (!p->orphaned) {
        p->orphaned = true;
        p->orphan_ms = now;
      }
      if (now - p->orphan_ms > ORPHAN_MS) {
        player_free(i);
        continue;
      }
    } else {
      p->orphaned = false;
      bool visible = now - p->seen_ms <= ONSCREEN_MS;
      if (visible && (int)(now - p->next_ms) >= 0) {
        unsigned delay;
        if (!imganim_step(p->a, &delay)) {
          player_free(i);
          continue;
        }
        publish(p);
        p->next_ms = now + delay;
      }
    }
    i++;
  }
  mutex_unlock(&g_lock);
}
