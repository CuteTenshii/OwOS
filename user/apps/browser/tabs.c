#include "main_int.h"

struct tab tabs[MAX_TABS];
int n_tabs = 1;
int active_tab = 0;

/* Free every heap buffer a page owns and zero it (so a reused slot starts clean) */
static void page_free(struct page *p)
{
  free(p->raw);
  free(p->text);
  free(p->attr);
  free(p->bp);
  free(p->align);
  free(p->fsz);
  free(p->lh);
  free(p->ls);
  free(p->va);
  free(p->fg);
  free(p->bg);
  free(p->links);
  free(p->regs);
  free(p->imgs);
  free(p->img_save);
  for (int i = 0; i < p->n_bgimgs; i++)
    free(p->bgimgs[i].rgba);
  free(p->bgimgs);
  for (int i = 0; i < p->n_faces; i++) {
    if (p->faces[i].font)
      font_free(p->faces[i].font);
    free(p->faces[i].data);
  }
  free(p->faces);
  if (p->csp)
    csp_free(p->csp);
  free(p->perm_policy_hdr);
  storage_drop_session(p->sid);
  memset(p, 0, sizeof *p);
}

/* The shared 16 MiB image scratch holds only the ACTIVE tab's images */
static void page_save_images(struct page *p)
{
  int n = B->img_pool_len;
  if (n <= 0) {
    p->img_save_len = 0;
    return;
  }
  unsigned char *s = realloc(p->img_save, (unsigned)n);
  if (!s) {
    p->img_save_len = 0;
    return;
  } /* OOM: images show their alt box */
  memcpy(s, B->img_pool, (unsigned)n);
  p->img_save = s;
  p->img_save_len = n;
}
static void page_restore_images(struct page *p)
{
  B->img_pool_len = 0;
  if (p->img_save_len > 0 && p->img_save_len <= (int)sizeof B->img_pool) {
    memcpy(B->img_pool, p->img_save, (unsigned)p->img_save_len);
    B->img_pool_len = p->img_save_len;
  }
}

/* Set while a fetch was started by Back/Next so its completion doesn't push a new history entry */
bool nav_from_history;

/* Free a tab's whole history (on close). */
static void hist_clear(struct tab *t)
{
  for (int i = 0; i < t->hist_n; i++) {
    free(t->hist[i]);
    t->hist[i] = 0;
  }
  t->hist_n = 0;
  t->hist_i = 0;
}

/* Record `url` as the tab's current page */
void hist_push(struct tab *t, const char *url)
{
  if (!url || !url[0])
    return;
  if (t->hist_n > 0 && streq(t->hist[t->hist_i], url))
    return; /* same page */

  for (int i = t->hist_i + 1; i < t->hist_n; i++) {
    free(t->hist[i]);
    t->hist[i] = 0;
  }
  t->hist_n = t->hist_i + 1;

  int n = (int)strlen(url) + 1;
  char *dup = malloc(n);
  if (!dup)
    return; /* OOM: just don't record */
  memcpy(dup, url, n);

  if (t->hist_n >= HIST_MAX) { /* full: drop the oldest */
    free(t->hist[0]);
    for (int i = 1; i < t->hist_n; i++)
      t->hist[i - 1] = t->hist[i];
    t->hist_n--;
  }
  t->hist[t->hist_n++] = dup;
  t->hist_i = t->hist_n - 1;
}

bool hist_can_back(const struct tab *t) { return t->hist_i > 0; }
bool hist_can_fwd(const struct tab *t) { return t->hist_i < t->hist_n - 1; }

/* Stop any in-flight load and wait for the worker to go idle, so it's safe for the main thread */
static void load_quiesce(void)
{
  B->req_pending = 0;
  B->abort_load = 1;
  while (B->loading || B->in_scripts)
    msleep(2); /* also wait out a background script pass */
  B->abort_load = 0;
}

/* Park the worker at the top of its loop so the main thread can mutate the page set */
static void worker_park(void)
{
  B->worker_pause = 1;
  while (!B->worker_idle)
    msleep(1);
}
static void worker_unpark(void) { B->worker_pause = 0; }

/* Leaving the active tab */
static void tab_leave(int i)
{
  struct tab *t = &tabs[i];
  B->drop_dom_tab = i; /* the tab whose DOM the worker is about to free */
  B->drop_dom = 1;
  B->events_live = 0;         /* worker frees the live DOM; page goes static */
  devtools_view_save(&t->dt); /* remember this tab's devtools view */
  page_save_images(&t->page);
}

/* --------------------------------------------------------------- input */

void pane_metrics(int *py, int *phh, int *rows)
{
  *py = MENU_H + TAB_H + ADDR_H;
  *phh = BH - MENU_H - TAB_H - ADDR_H;
  if (*phh < 0)
    *phh = 0;
  *rows = (*phh > 0) ? *phh / LINE_H : 1;
}

/* Apply a target scroll line, clamped to the page, repainting only when it actually moves */
void scroll_set(int target)
{
  int max = scroll_max();
  if (target > max)
    target = max;
  if (target < 0)
    target = 0;
  if (target != B->pg->scroll) {
    B->pg->scroll = target;
    g_dmg.bumps++;
    B->seq++;
  }
}

void navigate(const char *url)
{
  hover_url[0] = '\0'; /* drop any stale hovered-link status */
  if (gfx_sel_has(&g_sel))
    gfx_sel_clear(&g_sel);
  g_selecting = false;
  textfield_set(&B->addr_tf, url); /* writes into B->addr (its buffer) */
  int i = 0;
  for (; url[i] && i < URL_MAX - 1; i++)
    B->req_url[i] = url[i];
  B->req_url[i] = '\0';
  B->req_method[0] = '\0';
  B->req_body_len = 0; /* GET */
  B->req_pending = 1;
  B->seq++;
}

void blur_content_control(void)
{
  if (!B->focus_kind)
    return;
  int k = 0;
  sapp(B->evt_type, &k, sizeof B->evt_type, "blur");
  B->evt_pending = 1;
  B->focus_kind = 0;
}

void submit(void)
{
  if (B->addr_tf.len == 0)
    return;
  int i = 0;
  for (; B->addr[i] && i < URL_MAX - 1; i++)
    B->req_url[i] = B->addr[i];
  B->req_url[i] = '\0';
  B->req_method[0] = '\0';
  B->req_body_len = 0; /* GET */
  B->req_pending = 1;
  B->seq++;
}

/* Reload the current page (reload button, Page>Reload, F5, Ctrl+R) */
void do_reload(void)
{
  if (B->pg->cur_url[0])
    navigate(B->pg->cur_url);
}

/* Stop the in-flight load */
void do_stop(void)
{
  B->req_pending = 0;
  B->abort_load = 1;
  B->seq++;
}

/* Navigate to the active tab's previous/next history entry */
void go_back(void)
{
  struct tab *t = &tabs[active_tab];
  if (!hist_can_back(t))
    return;
  t->hist_i--;
  nav_from_history = true;
  navigate(t->hist[t->hist_i]);
}

void go_fwd(void)
{
  struct tab *t = &tabs[active_tab];
  if (!hist_can_fwd(t))
    return;
  t->hist_i++;
  nav_from_history = true;
  navigate(t->hist[t->hist_i]);
}

/* Reset B to an empty page (a fresh/blank tab) without touching the net. */
static void show_blank(void)
{
  B->drop_dom = 1;
  B->events_live = 0;
  textfield_set(&B->addr_tf, "");
  B->pg->cur_url[0] = '\0';
  B->pg->doc_title[0] = '\0';
  B->pg->mode = MODE_HTML;
  B->pg->text_len = 0;
  g_fr->lt.n = 0;
  B->pg->n_imgs = 0;
  B->pg->nlinks = 0;
  B->pg->nregs = 0;
  B->pg->page_bg = 0xffffff;
  B->pg->page_fg = 0x202830; /* default canvas, not the last page's */
  B->img_pool_len = 0;
  B->layout_dirty = 1;
  B->pg->favicon_ok = 0; /* a blank tab shows the globe */
  B->seq++;
}

/* Pull the freshly-loaded page's url + a label into the active tab */
void tab_sync_active(void)
{
  struct tab *t = &tabs[active_tab];
  strlcpy(t->url, B->pg->cur_url, sizeof t->url);
  if (B->pg->doc_title[0]) {
    strlcpy(t->label, B->pg->doc_title, sizeof t->label);
  } else {
    struct purl u;
    if (B->pg->cur_url[0] && parse_url(B->pg->cur_url, &u))
      strlcpy(t->label, u.host, sizeof t->label);
    else
      strlcpy(t->label, "Untitled", sizeof t->label);
  }
}

/* Reveal tab `i` */
static void reveal_tab(int i)
{
  B->pg = &tabs[i].page;           /* the swap , buffers already live here */
  devtools_view_load(&tabs[i].dt); /* this tab's devtools view; drops old capture */
  page_restore_images(&tabs[i].page);
  B->events_live = 0; /* revealed page has no live DOM        */
  B->loading = 0;
  B->req_pending = 0;
  B->layout_dirty = 1; /* force a re-wrap at the live width     */
  textfield_set(&B->addr_tf, tabs[i].url[0] ? tabs[i].url : tabs[i].page.cur_url);
  update_title(B->win_id);
  B->seq++;
  /* A crashed tab keeps its sad page (its empty text isn't "interrupted mid-load"), so don't */
  if (!tabs[i].crashed && tabs[i].page.text_len == 0 && tabs[i].url[0])
    navigate(tabs[i].url);
}

/* Make tab `i` the active one */
void select_tab(int i)
{
  if (i < 0 || i >= n_tabs || i == active_tab)
    return;
  load_quiesce(); /* worker idle before we move B->pg     */
  worker_park();  /* ...and fenced out of the pump too    */
  tab_leave(active_tab);
  active_tab = i;
  reveal_tab(i);
  worker_unpark();
}

void new_tab(void)
{
  if (n_tabs >= MAX_TABS) {
    B->seq++;
    return;
  }
  load_quiesce();
  worker_park();
  tab_leave(active_tab);
  active_tab = n_tabs++;
  struct tab *t = &tabs[active_tab];
  page_free(&t->page);        /* recycle the slot: free any stale page, zero it */
  devtools_view_free(&t->dt); /* and any stale devtools capture */
  t->url[0] = '\0';
  t->hist_n = 0;
  t->hist_i = 0;
  for (int k = 0; k < HIST_MAX; k++)
    t->hist[k] = 0;
  t->dt.open = 0;
  t->dt.tab = 0; /* devtools closed   */
  for (int k = 0; k < DT_PANELS; k++)
    t->dt.scroll[k] = 0;
  B->pg = &t->page; /* swap to the fresh page */
  devtools_view_load(&t->dt);
  strlcpy(t->label, "New Tab", sizeof t->label);
  show_blank();
  update_title(B->win_id); /* clear the previous tab's title */
}

void close_tab(int i)
{
  if (i < 0 || i >= n_tabs || n_tabs <= 1)
    return; /* keep at least one */
  int was_active = (i == active_tab);
  if (was_active)
    load_quiesce(); /* only the active tab loads: stop its fetch first */
  /* Fence the worker */
  worker_park();
  if (was_active)
    devtools_reset_live();             /* its live devtools capture isn't in .cap */
  page_free(&tabs[i].page);            /* free its page buffers   */
  devtools_view_free(&tabs[i].dt);     /* and its devtools capture */
  hist_clear(&tabs[i]);                /* and its history strings */
  for (int j = i; j < n_tabs - 1; j++) /* shift the rest down a slot (moves pages) */
    memcpy(&tabs[j], &tabs[j + 1], sizeof(struct tab));
  memset(&tabs[n_tabs - 1], 0, sizeof(struct tab)); /* vacated slot: drop the aliases */
  n_tabs--;
  if (active_tab > i)
    active_tab--;
  else if (active_tab >= n_tabs)
    active_tab = n_tabs - 1;
  B->pg = &tabs[active_tab].page; /* tabs moved: repoint at the active page */
  if (was_active)
    reveal_tab(active_tab); /* reveal the neighbour */
  worker_unpark();
  B->seq++;
}
