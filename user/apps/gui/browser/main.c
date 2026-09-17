/* main.c — the browser's windowed UI and event loop.
 *
 * Owns the one `struct browser` instance and the colour palette. The address
 * bar, content pane and scrollbar are drawn here; a worker thread
 * (thread_create) runs the blocking fetch so typing/scrolling stay live while a
 * page loads. The worker only touches B's buffers + net; window syscalls
 * (title, present) are made by this, the owning, thread.
 */
#include "anim.h"      /* CSS @keyframes / transition engine (drives per-frame relayout) */
#include "bmedia.h"    /* <audio>/<video> controls widget + playback state */
#include "bookmarks.h" /* persistent (url,title) bookmarks store */
#include "browser.h"
#include "cache.h"     /* persistent HTTP response cache: load at startup */
#include "clipboard.h" /* clip_put_text for Copy */
#include "dialog.h"    /* window.alert/confirm/prompt run on the main thread here */
#include "font.h"      /* custom @font-face faces (user/lib/font) */
#include "layout.h"    /* the CSS box tree (backgrounds/borders behind the text) */

static struct browser binst;
struct browser *B = &binst;

/* Content-pane text selection (drag to select, Ctrl+C to copy). The gfx_sel
 * model works in (display-line, byte-in-line) coordinates; g_selecting is set
 * between a press on plain text and the end of the drag. */
static struct gfx_sel g_sel;
static int g_selecting;

int BW = 600, BH = 368; /* client size; grows/shrinks on resize */

const unsigned palette[16] = {
    0x202830, 0x1a4fbf, 0xc01818, 0x108010, 0x9020a0, 0x9a6020, 0x107c90, 0x586070,
    0x16387a, 0x000000, 0x707000, 0xd07000, 0x808890, 0x304060, 0x2050d0, 0xa0a0a0,
};

/* The app menu bar: "Page" and "View" laid out on one bar via the gfx menubar
 * helper (one open at a time). It also exercises the widget's per-item states:
 * "Stop" is DISABLED while idle, "Show Images" is a CHECKED toggle, and
 * "Scroll" is a submenu holding the scroll commands. */
static int show_images = 1; /* "View > Show Images" checkbox state */

static const char *const page_items[] = {"Reload", "Home", "Stop"};
static const char *const page_hints[] = {"F5", "", ""};
static const unsigned char page_keys[] = {KEY_F5, 0, 0};
static unsigned char page_flags[] = {0, 0, MENU_DISABLED}; /* Stop: idle */
static struct menu pagemenu = {"Page", page_items, 3, 0, page_hints, page_keys, page_flags, 0};

/* "View > Scroll" submenu (opened by the View "Scroll" item). */
static const char *const scroll_items[] = {"Top", "Bottom", "Page Up", "Page Down"};
static struct menu scrollmenu = {"Scroll", scroll_items, 4, 0, 0, 0, 0, 0};

static const char *const view_items[] = {"Show Images", "Scroll"};
static unsigned char view_flags[] = {MENU_CHECKED, 0}; /* images on */
static struct menu *const view_subs[] = {0, &scrollmenu};
static struct menu viewmenu = {"View", view_items, 2, 0, 0, 0, view_flags, view_subs};

/* "Edit" menu: Copy (the text selection) + Select All. Copy dims with no
 * selection (render() updates edit_flags[0]). copy_selection/select_all are
 * defined below. */
static void copy_selection(void);
static void select_all(void);
static const char *const edit_items[] = {"Copy", "Select All"};
static unsigned char edit_flags[] = {MENU_DISABLED, 0};
static struct menu editmenu = {"Edit", edit_items, 2, 0, 0, 0, edit_flags, 0};

/* "Bookmarks" menu. The favicon/folder tree lives in a dedicated panel (drawn
 * by this file), not the flat gfx dropdown; this bar menu just holds the quick
 * commands. Item 0 toggles the current page's bookmark, item 1 shows/hides the
 * panel, item 2 makes a new top-level folder. The two toggle labels + item 0's
 * enabled state are refreshed per-open by bookmarks_menu_sync. */
static const char *bm_items[3];
static unsigned char bm_flags[3];
static char bm_toggle_label[24]; /* "Add"/"Remove Bookmark"  */
static char bm_show_label[20];   /* "Show"/"Hide Bookmarks"  */
static struct menu bmarkmenu = {"Bookmarks", bm_items, 3, 0, 0, 0, bm_flags, 0};

static struct menu *const bar_menus[] = {&pagemenu, &editmenu, &viewmenu, &bmarkmenu};
static struct menubar menubar = {bar_menus, 4, 2, 0};

/* --- tabs ------------------------------------------------------------------
 */
/* Each tab keeps its URL, label and scroll offset, plus — when it isn't the
 * active tab — a *snapshot* of its rendered page so switching back is instant
 * and never re-hits the network. Only the active tab's page lives in B (the
 * growable working buffers); an inactive tab's page is serialized into a single
 * heap blob the tab owns (malloc'd on cache, freed on restore/close). The blob
 * packs every variable-sized part — text, attr, img_pool, raw, the link list,
 * and the image list — back to back; the small fixed metadata (lengths, urls,
 * favicon) lives in the tab struct. A snapshot that can't be allocated falls
 * back to the old behaviour: re-fetch on select. There is no shared arena and
 * no eviction — each tab's blob is independent and demand-paged, so the only
 * bound on a cached page is available memory. The strip is the gfx tabbar
 * widget; this owns the list. */
#define MAX_TABS 8
#define HIST_MAX 32 /* per-tab back/forward history depth */
struct tab {
  char url[URL_MAX]; /* address-bar / navigation target               */
  char label[40];    /* tab-strip label (page title or host)          */
  struct page page;  /* this tab's whole page; B->pg points here when
                      * active, so a switch is a pointer swap (no copy) */

  /* Per-tab navigation history: URLs landed on, with hist_i the current entry.
   * Back/Next move hist_i and re-navigate; a fresh navigation truncates any
   * forward entries. Entries are malloc'd; the oldest is dropped when full. */
  char *hist[HIST_MAX];
  int hist_n; /* number of entries in use                    */
  int hist_i; /* index of the current page (0..hist_n-1)     */

  struct dt_view dt; /* this tab's devtools view (panel/scroll/open) */

  /* Set when this tab's page faulted (JS-runtime/GC OOM, a use-after-free, deep
   * recursion overrun): the worker caught the CPU fault, tore the page down,
   * and flagged it so render() shows the "tab crashed" page instead of the
   * browser dying. Cleared when the tab is navigated/reloaded. crash_sig is the
   * fault signal (SIGSEGV/SIGILL/SIGFPE …) that fired, used to label the page.
   */
  int crashed;
  int crash_sig;
};
static struct tab tabs[MAX_TABS];
static int n_tabs = 1;
static int active_tab = 0;

static void update_title(int id);      /* defined below; called from tab reveal */
static void navigate(const char *url); /* defined below; called from panels/handlers */

/* Free every heap buffer a page owns and zero it (so a reused slot starts
 * clean). Each tab owns its page, so this runs when a tab closes or a slot is
 * recycled. */
static void page_free(struct page *p)
{
  free(p->raw);
  free(p->text);
  free(p->attr);
  free(p->bp);
  free(p->fsz);
  free(p->lh);
  free(p->fg);
  free(p->bg);
  free(p->links);
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
  memset(p, 0, sizeof *p);
}

/* The shared 16 MiB image scratch holds only the ACTIVE tab's images.
 * Save/restore a tab's bytes around a switch so a backgrounded tab keeps its
 * images (compactly, sized to what it used) without refetching. */
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

/* Set while a fetch was started by Back/Next so its completion doesn't push a
 * new history entry (we're moving within the existing list, not extending it).
 */
static int nav_from_history;

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

/* Record `url` as the tab's current page. A reload of the same page is ignored;
 * any forward entries are dropped; the oldest is evicted when the list is full.
 */
static void hist_push(struct tab *t, const char *url)
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

static int hist_can_back(const struct tab *t) { return t->hist_i > 0; }
static int hist_can_fwd(const struct tab *t) { return t->hist_i < t->hist_n - 1; }

/* Stop any in-flight load and wait for the worker to go idle, so it's safe for
 * the main thread to repoint B->pg at another tab (the worker only ever writes
 * the active tab's page). Bounded: a load aborts as soon as its next read
 * bails. */
static void load_quiesce(void)
{
  B->req_pending = 0;
  B->abort_load = 1;
  while (B->loading || B->in_scripts)
    msleep(2); /* also wait out a background script pass */
  B->abort_load = 0;
}

/* Park the worker at the top of its loop so the main thread can mutate the page
 * set (B->pg, active_tab, tabs[]) with the worker guaranteed not to be reading
 * it. Call after load_quiesce(): that stops any in-flight fetch/script pass, so
 * the worker reaches the park point promptly; this then also fences the
 * timer/event/ media pump, the live window load_quiesce leaves open. Pair with
 * worker_unpark(). */
static void worker_park(void)
{
  B->worker_pause = 1;
  while (!B->worker_idle)
    msleep(1);
}
static void worker_unpark(void) { B->worker_pause = 0; }

/* Leaving the active tab: its page (buffers, colours, everything) stays put in
 * tabs[i].page — only the shared image scratch and the live DOM are tied to
 * "the active tab", so save the images and drop the DOM (the page becomes
 * static until revealed again). No buffer copy: the swap of B->pg in reveal_tab
 * does the rest. */
static void tab_leave(int i)
{
  struct tab *t = &tabs[i];
  B->drop_dom_tab = i; /* the tab whose DOM the worker is about to free */
  B->drop_dom = 1;
  B->events_live = 0;         /* worker frees the live DOM; page goes static */
  devtools_view_save(&t->dt); /* remember this tab's devtools view */
  page_save_images(&t->page);
}

/* --- worker thread: drains req_pending, fetches, bumps seq for the UI ----- */
static void rewrap(int paw); /* line-table flow; owned by the worker (see below) */
/* 256 KiB: http_txn holds hdr[4096] + req[2048] + a cookie buffer, and the
 * redirect follower stacks several URL_MAX (2 KiB) scratch URLs. But the deeper
 * consumer is the recursive DOM render walk (walk_element_inner): it carries a
 * ~552-byte `struct css_style` BY VALUE per frame, so a deeply nested page
 * (hundreds of elements) needs hundreds of KiB or it overflows into B's .bss
 * and corrupts it — surfacing as a browser page-fault AND a worker free() crash
 * at once. Bumped from 64 KiB; ~400 levels of nesting now fit. */
static char worker_stack[262144];
static volatile int worker_quit;

/* Lowest native SP the JS engine may recurse to on the worker thread before it
 * must throw a RangeError. The interpreter tree-walks + runs native callbacks
 * on this fixed stack; without a floor a deeply recursive script overruns it
 * into B's .bss + the heap (a browser page-fault AND a worker free() crash at
 * once). The headroom must cover the throw/Error-construction path and one deep
 * native frame. */
#define JS_STACK_HEADROOM 32768
void *browser_js_stack_floor(void) { return &worker_stack[JS_STACK_HEADROOM]; }

/* --- crash recovery: keep one tab's fault from taking down the whole browser
 * --- * All page work — parse, layout, and the JS runtime + GC — runs on the
 * worker thread. A JS-runtime/GC OOM dereferences a NULL allocation and
 * page-faults; a pathological page can also blow the stack or trip a
 * use-after-free. Left alone that fault kills the whole browser process (every
 * tab). Instead the worker installs a fault-signal handler (the kernel now
 * delivers a ring-3 fault as SIGSEGV/SIGILL/SIGFPE when a handler is set) that
 * longjmp()s back to a recovery point at the top of the worker loop, where the
 * crashed tab is flagged and its page torn down. The main (UI) thread never
 * runs JS, so it stays responsive. */
static jmp_buf worker_recovery;
static volatile int worker_in_recovery; /* re-entrancy guard: a fault while tearing
                                         * a crashed page down must not loop forever */

/* The tab whose page the worker is operating on this loop iteration — derived
 * from B->pg (the page the work actually touches), NOT the UI thread's
 * `active_tab`. The recovery point must flag THIS tab, because the main thread
 * can advance `active_tab` (opening/switching a tab) while a fault unwinds: a
 * page's own timer/event/media callback can fault in the pump section where
 * `load_quiesce` doesn't block the UI thread, so `active_tab` may already point
 * at a fresh tab by the time recovery runs. Snapshotting the worker's tab keeps
 * the sad page on the tab that actually crashed. */
static volatile int worker_tab;

/* The tab slot backing page `p` (B->pg always points into some tabs[i].page).
 */
static int tab_of_page(const struct page *p)
{
  for (int i = 0; i < n_tabs; i++)
    if (&tabs[i].page == p)
      return i;
  return active_tab; /* unreachable: B->pg is always a live tab's page */
}

static void worker_crash_handler(int sig)
{
  /* Carry the signal out as the setjmp return value so the recovery point knows
   * WHICH fault fired (OOM/bad access vs illegal instruction vs FP error) and
   * can label the crash page. Every fault signal is non-zero, so setjmp reads
   * it as "crashed". Never returns — the faulting instruction would just
   * re-fault. */
  longjmp(worker_recovery, sig);
}

static void worker_install_crash_handlers(void)
{
  signal(SIGSEGV, worker_crash_handler);
  signal(SIGILL, worker_crash_handler);
  signal(SIGFPE, worker_crash_handler);
  signal(SIGBUS, worker_crash_handler);
  signal(SIGABRT, worker_crash_handler);
}

/* After a longjmp out of a fault: re-arm delivery and release any lock the
 * unwound frame was holding, so the browser doesn't deadlock on the next
 * allocation/paint. Ordered so nothing here can itself fault before delivery is
 * re-armed. */
static void worker_after_crash(void)
{
  browser_buf_unlock(); /* the realloc/render interlock       */
  heap_lock_reset();    /* the malloc spinlock                */
  /* The kernel masked the fault signal to run the (never-returning) handler;
   * unblock it or the next tab crash would be uncatchable and fatal. */
  unsigned m = sigmask(SIGSEGV) | sigmask(SIGILL) | sigmask(SIGFPE) | sigmask(SIGBUS) | sigmask(SIGABRT);
  sigprocmask(SIG_UNBLOCK, &m, 0);
}

static void worker_navigate(const char *url)
{
  int k = 0;
  for (; url[k] && k < URL_MAX - 1; k++)
    B->req_url[k] = url[k];
  B->req_url[k] = '\0';
  B->req_method[0] = '\0'; /* GET: clear any prior POST */
  B->req_body_len = 0;
  B->req_body[0] = '\0';
  B->req_ctype[0] = '\0';
  B->req_pending = 1;
  B->seq++;
}

/* Queue a POST navigation (a form submission). Same handoff as worker_navigate,
 * plus the request body + its Content-Type; do_fetch sends them and clears the
 * method afterward so the next navigation defaults back to GET. */
static void worker_navigate_post(const char *url, const char *body, int blen, const char *ctype)
{
  int k = 0;
  for (; url[k] && k < URL_MAX - 1; k++)
    B->req_url[k] = url[k];
  B->req_url[k] = '\0';
  strcpy(B->req_method, "POST");
  if (blen > (int)sizeof B->req_body - 1)
    blen = (int)sizeof B->req_body - 1;
  if (blen < 0)
    blen = 0;
  memcpy(B->req_body, body, (unsigned)blen);
  B->req_body[blen] = '\0';
  B->req_body_len = blen;
  strcpy(B->req_ctype, ctype ? ctype : "application/x-www-form-urlencoded");
  B->req_pending = 1;
  B->seq++;
}

/* Submit the form enclosing `node`: optionally fire 'submit', and unless a
 * handler prevented it, navigate (GET query or POST body) to the built target.
 * Returns 1 if a form was found. Shared by interactive submits and the JS form
 * API. */
int browser_form_submit(struct dom_node *node, int fire_event)
{
  struct form_submission fs;
  struct dom_node *form;
  for (form = node; form; form = form->parent)
    if (streq(form->tag, "form"))
      break;
  if (!form)
    return 0;
  if (fire_event && js_dispatch_dom_event(form, "submit", 0, 0))
    return 1; /* preventDefault */
  if (html_form_build(node, &fs) && fs.url[0]) {
    if (streq(fs.method, "POST"))
      worker_navigate_post(fs.url, fs.body, fs.body_len, fs.ctype);
    else
      worker_navigate(fs.url);
  }
  return 1;
}

static int worker_submit(struct dom_node *from) { return browser_form_submit(from, 1); }

/* Perform a clicked control's default action: toggle/cycle/focus/submit, firing
 * the matching DOM events. Returns 1 if the page should re-flatten afterwards.
 */
static int activate_control(struct dom_node *c, int kind)
{
  switch (kind) {
  case CTRL_CHECKBOX:
    html_toggle_checked(c);
    js_dispatch_dom_event(c, "change", 0, 0);
    html_set_focus(0);
    return 1;
  case CTRL_RADIO:
    html_radio_select(c);
    js_dispatch_dom_event(c, "change", 0, 0);
    html_set_focus(0);
    return 1;
  case CTRL_SELECT:
    html_select_cycle(c);
    js_dispatch_dom_event(c, "change", 0, 0);
    html_set_focus(0);
    return 1;
  case CTRL_TEXT:
    html_set_focus(c);
    return 1;
  case CTRL_TEXTAREA:
    if (!dom_get_attr(c, "value"))
      html_seed_textarea(c);
    html_set_focus(c);
    return 1;
  case CTRL_SUBMIT:
    html_set_focus(0);
    worker_submit(c);
    return 1;
  default:
    html_set_focus(0);
    return 1;
  }
}

/* Run a queued content-pane event through the live JS DOM (worker-thread only —
 * the engine is single-threaded). For a click: resolve the target node,
 * dispatch the click, then unless a handler called preventDefault perform the
 * default action (activate a form control, or follow a link). For keyinput:
 * edit the focused text control and fire an 'input' event. Re-flatten if
 * anything changed. */
static void worker_dispatch_event(void)
{
  unsigned mseq = dom_mutation_seq;
  int need_relayout = 0;
  B->evt_pending = 0;

  if (streq(B->evt_type, "blur")) {
    if (html_focus()) {
      html_set_focus(0);
      html_relayout();
    }
    return;
  }
  if (streq(B->evt_type, "keyinput")) {
    struct dom_node *f = html_focus(); /* evt_x carries the char */
    int ch = B->evt_x;
    if (f && (ch == '\n' || ch == '\r') && html_control_kind(f) == CTRL_TEXT) {
      worker_submit(f); /* Enter in a single-line field */
      need_relayout = 1;
    } else if ((f = html_focus_edit(ch)) != 0) {
      js_dispatch_dom_event(f, "input", 0, 0);
      need_relayout = 1;
    }
  } else { /* a click */
    struct dom_node *t = html_node_at_offset(B->evt_off), *ctrl;
    int prevented = js_dispatch_dom_event(t, "click", B->evt_x, B->evt_y);
    if (!prevented) {
      int kind = CTRL_NONE;
      for (ctrl = t; ctrl; ctrl = ctrl->parent)
        if ((kind = html_control_kind(ctrl)) != CTRL_NONE)
          break;
      if (ctrl)
        need_relayout |= activate_control(ctrl, kind);
      else {
        if (html_focus()) {
          html_set_focus(0);
          need_relayout = 1;
        } /* blur */
        if (B->evt_text[0])
          worker_navigate(B->evt_text); /* link */
      }
    }
  }

  if (dom_mutation_seq != mseq || need_relayout) {
    html_relayout();
    if (B->pg->n_imgs > 0)
      fetch_inline_images(B->pg->cur_url);
  }
  B->events_live = js_has_listeners() || html_has_controls();
}

static void worker_fn(void)
{
  worker_install_crash_handlers();

  /* Recovery point. A CPU fault anywhere in the page work below (JS/GC OOM, a
   * stack overrun, a use-after-free) delivers a signal whose handler longjmp()s
   * here. Flag the tab the worker was actually rendering (worker_tab, not the
   * UI thread's active_tab) so render() shows the "tab crashed" page on the tab
   * that crashed, then reclaim the (now-dead) page so the browser keeps
   * running. */
  int crash_sig = setjmp(worker_recovery);
  if (crash_sig) {
    worker_after_crash();
    tabs[worker_tab].crashed = 1;
    tabs[worker_tab].crash_sig = crash_sig;
    B->loading = 0;
    B->abort_load = 0;
    B->scripts_pending = 0;
    B->in_scripts = 0;
    B->evt_pending = 0;
    B->events_live = 0;
    B->pg->text_len = 0;
    B->nlines = 0; /* the sad-tab page ignores these */
    B->seq++;
    if (!worker_in_recovery) {
      /* First fault for this page: reclaim its DOM/JS heap. Guarded — if the
       * teardown itself refaults on a corrupted heap we land back here with the
       * flag set and skip it, leaking the page rather than looping forever. */
      worker_in_recovery = 1;
      html_page_teardown();
      worker_in_recovery = 0;
    } else {
      worker_in_recovery = 0; /* give up on teardown; stay alive */
    }
  }

  for (;;) {
    if (worker_quit)
      exit(0);
    /* Park here when the main thread is mutating the page set
     * (opening/switching/ closing a tab): it needs B->pg / active_tab / tabs[]
     * to hold still while the worker is guaranteed not to be reading them. This
     * is the fence load_quiesce can't provide on its own — it stops in-flight
     * loads but not the pump below, so without this a B->pg swap could land
     * mid-timer/-event/-media callback. */
    if (B->worker_pause) {
      B->worker_idle = 1;
      while (B->worker_pause)
        msleep(1);
      B->worker_idle = 0;
    }
    /* Record which tab this iteration's work belongs to (the page B->pg
     * touches) before doing anything that can fault, so the recovery point
     * above flags the right tab even if the UI thread advances active_tab
     * meanwhile. */
    worker_tab = tab_of_page(B->pg);
    if (B->drop_dom) { /* main switched away: free the page  */
      B->drop_dom = 0;
      B->events_live = 0;
      /* This frees the DEPARTING tab's live DOM, but B->pg already points at
       * the tab we switched to — so blame a teardown fault on the tab that
       * owned the DOM, never the freshly-revealed one. */
      worker_tab = B->drop_dom_tab;
      html_page_teardown();
    }
    if (B->recascade) { /* window resized: re-run the cascade  */
      B->recascade = 0;
      html_relayout(); /* re-evaluates @media at the new size */
      if (B->pg->n_imgs > 0)
        fetch_inline_images(B->pg->cur_url);
      B->seq++;
    }
    if (B->req_pending) {
      B->req_pending = 0;
      B->abort_load = 0;            /* a fresh navigation clears any pending Stop */
      tabs[worker_tab].crashed = 0; /* reloading/navigating clears the crash page */
      B->loading = 1;
      B->seq++;
      do_fetch();
      /* An HTML page's scripts run on the NEXT pass (html_run_page_scripts),
       * once this laid-out page has been published + painted. Keep `loading`
       * set until then so load_quiesce still waits out the whole load. */
      if (!B->scripts_pending) {
        B->loading = 0;
        B->abort_load = 0;
        B->events_live = js_has_listeners() || html_has_controls();
      }
      B->seq++;
    } else if (B->scripts_pending) {
      /* Deferred <script> pass. The page painted on the previous pass with all
       * its assets (HTML, CSS, fonts, inline images, favicon) already fetched,
       * so the load is COMPLETE now — clearing `loading` finalizes it on the
       * main thread (title, URL, history, spinner off). The scripts then run in
       * the background with `loading` already cleared; `in_scripts` keeps
       * load_quiesce waiting so a tab switch can't swap the page mid-script.
       * A fresh navigation or Stop (abort_load) requested meanwhile skips them.
       */
      B->scripts_pending = 0;
      if (!B->abort_load) {
        B->in_scripts = 1; /* worker busy: load_quiesce must wait */
        B->loading = 0;
        B->abort_load = 0;
        B->seq++; /* main thread finalizes the load now */
        unsigned mseq = dom_mutation_seq;
        html_run_page_scripts();
        if (dom_mutation_seq != mseq) { /* a script mutated the DOM */
          html_relayout();
          if (B->pg->n_imgs > 0)
            fetch_inline_images(B->pg->cur_url);
        }
        cache_flush(); /* persist anything the scripts cached */
        B->events_live = js_has_listeners() || html_has_controls();
        B->in_scripts = 0;
      } else {
        B->loading = 0; /* aborted: just release load_quiesce */
      }
      B->seq++;
    } else if (B->evt_pending) {
      worker_dispatch_event();
    } else if (fetch_lazy_visible()) {
      continue; /* a deferred image landed: look again */
    }
    /* Drive the page's timers (setTimeout/setInterval/rAF) and live
     * WebSockets. A fired callback may mutate the DOM — relayout + repaint
     * when it did. */
    {
      unsigned mseq = dom_mutation_seq;
      /* media_pump fires HTMLMediaElement events (play/timeupdate/ended/...)
       * from the worker, the only thread allowed to touch the JS engine. */
      if (js_timers_pump() + js_websockets_pump() + js_notifications_pump() + media_pump() + js_cast_pump() +
          js_idb_pump()) {
        if (dom_mutation_seq != mseq) {
          html_relayout();
          if (B->pg->n_imgs > 0)
            fetch_inline_images(B->pg->cur_url);
        }
        B->seq++;
      }
    }
    /* Drive CSS animations/transitions: while any @keyframes animation or
     * transition is mid-flight, re-flatten the live DOM so anim_apply() samples
     * the next frame. The loop's msleep(30) below paces this to ~30fps; when
     * nothing is animating anim_active() is false and no work is done. */
    if (!B->loading && anim_active()) {
      html_relayout();
      B->seq++;
    }

    /* Load any <iframe> documents the last layout discovered: fetch each,
     * check it against X-Frame-Options / CSP frame-ancestors, then re-flatten
     * so the allowed ones render in place of their placeholders. A frame's own
     * content may itself hold iframes, surfaced on a later pass. */
    if (!B->req_pending && !B->loading && html_iframes_pending()) {
      fetch_iframes();
      html_relayout();
      if (B->pg->n_imgs > 0)
        fetch_inline_images(B->pg->cur_url);
      B->seq++;
    }

    /* Rebuild the devtools DOM/Styles snapshot from the live tree (this thread
     * owns it) when the panel is open and the page changed. No-op when closed.
     */
    devtools_worker_tick();

    /* Flow the line table on THIS thread, not in render(). A big page's reflow
     * (layout_flow over the whole document) is the one expensive step; keeping
     * it off the main thread means render() only paints the built table and the
     * event loop never stalls ("Not Responding"). Re-flow when the box
     * tree/text changed (layout_dirty) or the main thread published a new width
     * (resize). Held under browser_buf_lock so render() never observes a half-built
     * table. */
    if (B->want_paw > 0 && (B->layout_dirty || B->wrap_cols != B->want_paw)) {
      browser_buf_lock();
      rewrap(B->want_paw);
      browser_buf_unlock();
      B->seq++;
    }
    bookmarks_flush(); /* persist a changed bookmarks tree (deferred) */
    msleep(30);
  }
}

/* --------------------------------------------------------------- word wrap */

/* The page body is drawn with the PROPORTIONAL sans-serif UI font (gfx_ui_*),
 * not the monospace cell grid: a glyph's pen advance is gfx_ui_cw(c), a line is
 * LINE_H tall, and wrapping/hit-testing measure in pixels. (The address bar,
 * tabs and menus already use this font.) */
#define LINE_H UIFONT_H

/* --- body font: the baked proportional UI font, OR a page @font-face --------
 * * When the page loads a usable @font-face and applies it to the body
 * (B->body_ font != NULL), body text is drawn with that custom face via
 * user/lib/font; otherwise it falls back to the baked gfx_ui font. Custom
 * glyphs are rasterized on demand and cached per ASCII codepoint (the body text
 * is folded to 7-bit), at a fixed pixel size, so a repaint only blits. The
 * cache is main-thread-only; B->pg->body_font is swapped by the worker under
 * browser_buf_lock (see html.c). */
/* B->pg->text holds UTF-8; the renderer decodes a codepoint at a time via the
 * shared utf8_decode/utf8_seqlen (user/lib). Per-codepoint glyph cache for the
 * active direct-mapped table (evict on collision, so a CJK page with hundreds
 * of glyphs stays bounded and a repaint only blits). Main-thread-only;
 * B->pg->body_font is swapped by the worker under browser_buf_lock (see html.c). */
#define BODY_PX 15.0f
#define GCACHE_N 1024
struct gcache {
  unsigned cp;
  unsigned char *bm;
  int w, h, ox, oy, adv;
  int valid;
};
static struct gcache g_glyph[GCACHE_N];
static struct font *g_gf; /* the face the cache currently holds */
static int g_baseline;    /* baseline offset (px) within a line */

static void gcache_sync(void)
{
  if (g_gf == B->pg->body_font)
    return;
  for (int i = 0; i < GCACHE_N; i++) {
    free(g_glyph[i].bm);
    g_glyph[i].bm = 0;
    g_glyph[i].valid = 0;
  }
  g_gf = B->pg->body_font;
  if (g_gf) {
    int asc = 0;
    font_vmetrics(g_gf, BODY_PX, &asc, 0, 0);
    g_baseline = asc;
  }
}

static struct gcache *gcache_get(unsigned cp)
{
  struct gcache *g = &g_glyph[cp & (GCACHE_N - 1)];
  if (g->valid && g->cp == cp)
    return g;
  if (g->valid) {
    free(g->bm);
    g->bm = 0;
  }
  int gi = font_glyph_index(B->pg->body_font, (int)cp);
  g->bm = font_render_glyph(B->pg->body_font, gi, BODY_PX, &g->w, &g->h, &g->ox, &g->oy, &g->adv);
  g->cp = cp;
  g->valid = 1;
  return g;
}

/* Heading pixel sizes (BP_SIZE 1..6 = h1..h6); 0 = ordinary body text, 7 = a
 * horizontal rule. Kept within LINE_H (18) so a taller heading still fits one
 * row — headings also come bold (stamped in html.c), which carries the weight.
 */
static const int g_size_px[8] = {0, 18, 17, 16, 15, 14, 13, 0};

/* Block context for the paragraph currently being measured/drawn, decoded from
 * a glyph's BP_* byte by set_block(): a heading px (0 = default UI font),
 * monospace
 * (<pre>), centring, and an indent level (blockquote/list nesting). */
static int cur_bpx;     /* heading px, 0 = default body font   */
static int cur_bmono;   /* <pre>: render in the monospace cell */
static int cur_bcenter; /* text-align:center / <center>        */
static int cur_bindent; /* indent level 0..7                   */
static int cur_bhr;     /* this line is a horizontal rule      */
static int cur_blh;     /* CSS line-height px at the line start, 0 = auto */

/* Indent width in pixels for one nesting level. */
#define INDENT_PX 24

static void set_block(int off)
{
  int inb = (off >= 0 && off < B->pg->text_len);
  unsigned char bp = inb ? B->pg->bp[off] : 0;
  int fpx = inb ? B->pg->fsz[off] : 0; /* CSS font-size px (0 = unset) */
  int size = bp & BP_SIZE;
  cur_bhr = (size == 7);
  cur_bpx = fpx ? fpx : ((size >= 1 && size <= 6) ? g_size_px[size] : 0);
  cur_bmono = (bp & BP_MONO) ? 1 : 0;
  cur_bcenter = (bp & BP_CENTER) ? 1 : 0;
  cur_bindent = (bp & BP_INDENT) >> BP_INDENT_SH;
  cur_blh = inb ? B->pg->lh[off] : 0;
}

/* Per-glyph resolved font-size px (0 = default body) — like set_block's cur_bpx
 * but for one glyph, so a line with mixed inline sizes measures/draws each run
 * right. */
static int glyph_px(int off)
{
  if (off < 0 || off >= B->pg->text_len)
    return 0;
  int f = B->pg->fsz[off];
  if (f)
    return f;
  int sz = B->pg->bp[off] & BP_SIZE;
  return (sz >= 1 && sz <= 6) ? g_size_px[sz] : 0;
}

/* Pen advance of one codepoint in the active body font (baked UI font when no
 * custom face: ASCII/Latin-1 by value, a '?'-width box for anything above it).
 */
/* Raw, non-HTML content (text/plain, source, etc.) renders in the fixed-width
 * monospace cell font, not the proportional UI font — only HTML gets the
 * latter. A <pre> block (cur_bmono) is monospace too, even inside an HTML page.
 */
static int body_mono(void) { return B->pg->mode == MODE_TEXT || cur_bmono; }

static int body_cp_adv(unsigned cp, int px)
{
  if (body_mono())
    return GLYPH_W;      /* monospace cell             */
  if (!B->pg->body_font) /* the gfx system-ui font     */
    return px ? gfx_ui_cw_cp_px(cp, px) : gfx_ui_cw_cp(cp);
  if (px && px != (int)BODY_PX) { /* sized run: advance at px   */
    int gi = font_glyph_index(B->pg->body_font, (int)cp);
    int adv = 0;
    font_glyph_hmetrics(B->pg->body_font, gi, (float)px, &adv, 0);
    return adv;
  }
  gcache_sync();
  return gcache_get(cp)->adv;
}

/* Alpha-blend one cached glyph's coverage in colour fg, clipped to [ct,cb).
 * With `italic`, faux-slant: each row shifts horizontally by ~1/4 its height
 * above the baseline `y` (rows above the baseline lean right, below lean left).
 */
static void blit_glyph(int x, int y, const struct gcache *g, unsigned fg, int ct, int cb, int italic)
{
  if (!g->bm)
    return;
  unsigned *fb = browser_surface();
  unsigned fr = (fg >> 16) & 0xff, fgc = (fg >> 8) & 0xff, fbb = fg & 0xff;
  for (int gy = 0; gy < g->h; gy++) {
    int py = y + g->oy + gy;
    if (py < ct || py >= cb || py < 0 || py >= BH)
      continue;
    int shift = italic ? (y - py) / 4 : 0;
    const unsigned char *srow = g->bm + gy * g->w;
    unsigned *drow = fb + (unsigned long)py * BW;
    for (int gx = 0; gx < g->w; gx++) {
      int px = x + g->ox + gx + shift;
      if (px < 0 || px >= BW)
        continue;
      unsigned a = srow[gx];
      if (!a)
        continue;
      if (a == 255) {
        drow[px] = fg;
        continue;
      }
      unsigned dst = drow[px], ia = 255 - a;
      unsigned r = (fr * a + ((dst >> 16) & 0xff) * ia) / 255;
      unsigned gg = (fgc * a + ((dst >> 8) & 0xff) * ia) / 255;
      unsigned bb = (fbb * a + (dst & 0xff) * ia) / 255;
      drow[px] = (r << 16) | (gg << 8) | bb;
    }
  }
}

/* A "tofu" box for a codepoint the active font (or the ASCII baked font) can't
 * draw, so missing glyphs are visible rather than silently dropped. */
static void draw_tofu(int x, int y, int w, unsigned fg)
{
  if (w > 4)
    gfx_rect(x + 1, y + 3, w - 3, UIFONT_H - 6, fg);
}

/* Draw a UTF-8 string at line-top (x,y) in colour fg, one codepoint at a time,
 * clipped to [ct,cb). Returns the pen x after the string. With a custom face,
 * glyphs come from font.c; otherwise the baked UI font draws ASCII/Latin-1 and
 * a tofu box stands in for anything it lacks. */
/* Draw a UTF-8 run of font size `px` (0 = default body), its baseline
 * `baseline` px below line-top `y` so runs of different sizes on one line share
 * a baseline. Clipped to [ct,cb); returns the pen x after the run. */
static int body_draw_run(int x, int y, const char *s, int len, int width, unsigned fg, int ct, int cb, int px,
                         int baseline, int italic)
{
  const char *end = s + len;
  unsigned cp;
  if (body_mono()) { /* raw content: monospace cells */
    (void)ct;
    (void)cb;
    (void)baseline;
    gfx_text(x, y, s, fg, GFX_TRANSPARENT);
    return x + width;
  }
  if (B->pg->body_font) {
    int by = y + baseline; /* screen baseline shared by runs */
    gcache_sync();
    if (px && px != (int)BODY_PX) { /* sized run: render uncached at px */
      while (s < end) {
        s += utf8_decode(s, end, &cp);
        int gi = font_glyph_index(B->pg->body_font, (int)cp);
        struct gcache g;
        g.bm = font_render_glyph(B->pg->body_font, gi, (float)px, &g.w, &g.h, &g.ox, &g.oy, &g.adv);
        if (g.bm) {
          blit_glyph(x, by, &g, fg, ct, cb, italic);
          free(g.bm);
        } else if (cp > 0x20)
          draw_tofu(x, y, g.adv, fg);
        x += g.adv;
      }
      return x;
    }
    while (s < end) { /* default BODY_PX: cached glyphs */
      s += utf8_decode(s, end, &cp);
      struct gcache *g = gcache_get(cp);
      if (g->bm)
        blit_glyph(x, by, g, fg, ct, cb, italic);
      else if (cp > 0x20)
        draw_tofu(x, y, g->adv, fg); /* .notdef / empty */
      x += g->adv;
    }
    return x;
  }
  /* No @font-face: the gfx system-ui font (size-cached). gfx_ui_text_px puts
   * the baseline at y+ascent(px), so offset y to land it on the line's shared
   * baseline. */
  int eff = px ? px : (int)BODY_PX;
  gfx_ui_text_px_clip(x, y + baseline - gfx_ui_ascent_px(eff), s, fg, eff, ct, cb);
  return x + gfx_ui_width_px(s, eff);
}

/* Ascent (line-top → baseline) for a run of `px` in the active body font; a
 * line's baseline is the max over its runs, so mixed sizes share one baseline.
 */
static int run_ascent(int px)
{
  if (px <= 0)
    px = (int)BODY_PX;
  if (B->pg->body_font) {
    int a = 0;
    font_vmetrics(B->pg->body_font, (float)px, &a, 0, 0);
    return a;
  }
  return gfx_ui_ascent_px(px);
}
/* Largest effective font px among the glyphs of [off,end) (default = BODY_PX).
 */
static int line_maxpx(int off, int end)
{
  int mx = (int)BODY_PX;
  for (int p = off; p < end;) {
    int g = glyph_px(p);
    int eff = g ? g : (int)BODY_PX;
    if (eff > mx)
      mx = eff;
    p += utf8_seqlen((unsigned char)B->pg->text[p]);
  }
  return mx;
}
/* Display height of a text line. An explicit CSS line-height (max over the
 * line's glyphs) wins; otherwise it's derived from the largest font (≈1.25
 * line-height). */
static int line_height_for(int off, int end)
{
  int lhmax = 0;
  for (int p = off; p < end;) {
    int v = B->pg->lh[p];
    if (v > lhmax)
      lhmax = v;
    p += utf8_seqlen((unsigned char)B->pg->text[p]);
  }
  if (lhmax > 0)
    return lhmax < 6 ? 6 : lhmax;
  int mx = line_maxpx(off, end);
  if (mx <= (int)BODY_PX)
    return LINE_H;
  int lh = mx + (mx + 2) / 4;
  return lh < LINE_H ? LINE_H : lh;
}

/* While a flex-row item is being flowed, every wrapped line is captured into
 * the overlay on the band instead of appended to the main table (set by
 * flow_flex_row). */
static int g_ov_active; /* 1 = redirect add_line to the flex overlay */
static int g_ov_base;   /* absolute band row that the item's first line maps to */
static int g_ov_n;      /* lines emitted for the current item so far */

static void add_line(int off, int len, int x, int w, int h)
{
  if (g_ov_active) {
    int lh = h > 0 ? h : LINE_H;
    browser_ov_add(g_ov_base + g_ov_n, off, len, x, w, lh, line_maxpx(off, off + len));
    g_ov_n++;
    return;
  }
  if (!browser_ensure_lines(B->nlines + 1))
    return; /* out of memory: stop adding lines */
  int n = B->nlines;
  B->line_off[n] = off;
  B->line_len[n] = len;
  B->line_x[n] = x;
  B->line_w[n] = w;
  B->line_h[n] = h > 0 ? h : LINE_H;
  B->line_px[n] = line_maxpx(off, off + len);
  B->line_y[n] = n > 0 ? B->line_y[n - 1] + B->line_h[n - 1] : 0;
  B->nlines++;
}

/* ---- pixel geometry over the line table (Stage 6 pixel scroll) ---- *
 * Each display line carries its document-pixel top (line_y) and height
 * (line_h); `B->pg->scroll` is a document-pixel offset. These map between line
 * and pixel. */
static int line_bottom(int i) { return (i >= 0 && i < B->nlines) ? B->line_y[i] + B->line_h[i] : 0; }
static int doc_px(void) { return B->nlines > 0 ? line_bottom(B->nlines - 1) : 0; }
/* The line whose band contains document-pixel `y` (clamped to a real line). */
static int line_at_px(int y)
{
  int lo = 0, hi = B->nlines - 1, ans = 0;
  if (hi < 0 || y <= 0)
    return 0;
  while (lo <= hi) { /* last line with line_y <= y */
    int mid = (lo + hi) / 2;
    if (B->line_y[mid] <= y) {
      ans = mid;
      lo = mid + 1;
    } else
      hi = mid - 1;
  }
  return ans;
}

/* How many display rows an inline image reserves at the current pane width. */
/* Decide an inline image's drawn size (dw x dh px) within a pane `paw` px wide,
 * honouring the CSS/HTML width & height: both given pin the box (a single one
 * scales the other by the natural aspect ratio); with neither, the natural size
 * is used. The result is shrunk to fit the pane width, ratio preserved. dw/dh
 * are 0 when nothing can be sized yet (no explicit size and the image isn't
 * decoded), which the caller renders as a one-row alt box. Explicit sizes work
 * even before the image loads, so a lazy/slow image reserves its space and the
 * page doesn't reflow when it lands. */
static void img_disp_size(const struct inl_img *im, int paw, int *dw, int *dh)
{
  int natural = im->ok && im->iw && im->ih;
  int w = 0, h = 0;
  if (im->cw && im->ch) {
    w = im->cw;
    h = im->ch;
  } else if (im->cw) {
    w = im->cw;
    h = natural ? (int)((unsigned long)im->cw * im->ih / im->iw) : im->cw;
  } else if (im->ch) {
    h = im->ch;
    w = natural ? (int)((unsigned long)im->ch * im->iw / im->ih) : im->ch;
  } else if (natural) {
    w = (int)im->iw;
    h = (int)im->ih;
  } else {
    *dw = 0;
    *dh = 0;
    return;
  }
  if (w > paw) {
    h = (int)((long)h * paw / (w ? w : 1));
    w = paw;
  } /* fit width */
  *dw = w < 1 ? 1 : w;
  *dh = h < 1 ? 1 : h;
}

static void pane_metrics(int *py, int *phh, int *rows);
static void sel_hl_rect(int x, int y, int w, int h); /* selection highlight (defined below) */

/* Highest valid scroll line: the last `rows` lines fill the pane exactly, so
 * the bottom stop can't scroll content up into blank space below it. */
static int scroll_max(void)
{
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  int max = doc_px() - phh;
  return max < 0 ? 0 : max;
}

/* --- approximate element geometry for the DOM geometry APIs (js.c) ----------
 * * The line-flow renderer has no per-element box model, so an element's pixel
 * box is reconstructed from the wrapped line table + inline-image rows. Returns
 * a document-relative box (x,y,w,h) for the rendered text range [start,end), 0
 * if the range produced no rows (unrendered/empty). Width is exact for a range
 * that lies on one text line; the content column otherwise. Runs on the worker
 * (JS) thread, so it holds browser_buf_lock — render() rebuilds the line table and uses
 * the glyph cache (set_block/body_cp_adv) under the same lock. */
int browser_range_box(int start, int end, int *ox, int *oy, int *ow, int *oh)
{
  *ox = *oy = *ow = *oh = 0;
  if (end <= start)
    end = start + 1; /* zero-width span: one anchor cell */
  int paw = (BW - SCROLL_W) - 8;
  if (paw < 8)
    paw = 8;

  browser_buf_lock();
  int first = -1, last = -1, text_rows = 0, img_w = 0;
  for (int r = 0; r < B->nlines; r++) {
    int rs = B->line_off[r], rl = B->line_len[r];
    int hit = rl > 0 ? (rs < end && rs + rl > start) /* text-line overlap */
                     : (rs >= start && rs < end);    /* blank/anchor row  */
    if (!hit)
      continue;
    if (first < 0)
      first = r;
    last = r;
    if (rl > 0)
      text_rows++;
  }
  /* Inline images anchored inside the span: the <img> element itself (whose
   * span ends at its anchor) and any ancestor block enclosing it. */
  for (int q = 0; q < B->pg->n_imgs; q++) {
    struct inl_img *im = &B->pg->imgs[q];
    if (im->start_row < 0 || im->text_pos < start || im->text_pos > end)
      continue;
    int s0 = im->start_row, s1 = s0 + (im->rows > 0 ? im->rows : 1) - 1;
    if (first < 0 || s0 < first)
      first = s0;
    if (s1 > last)
      last = s1;
    int dw = 0, dh = 0;
    img_disp_size(im, paw, &dw, &dh);
    if (dw > img_w)
      img_w = dw;
  }
  if (first < 0) {
    browser_buf_unlock();
    return 0;
  }

  int x = B->line_x[first], w = B->line_w[first];
  int y = B->line_y[first], h = line_bottom(last) - B->line_y[first];
  if (first == last && text_rows == 1) { /* one text line: exact x + width */
    int rs = B->line_off[first], re = rs + B->line_len[first];
    const char *t = B->pg->text;
    unsigned cp;
    set_block(rs);
    x += cur_bindent * INDENT_PX;
    if (cur_bcenter) {
      int lw = 0;
      for (int p = rs; p < re;) {
        int gp = p;
        p += utf8_decode(&t[p], &t[re], &cp);
        lw += body_cp_adv(cp, glyph_px(gp));
      }
      int room = (B->line_w[first] - cur_bindent * INDENT_PX) - lw;
      if (room > 0)
        x += room / 2;
    }
    int a = start > rs ? start : rs, b = end < re ? end : re;
    for (int p = rs; p < a;) {
      int gp = p;
      p += utf8_decode(&t[p], &t[re], &cp);
      x += body_cp_adv(cp, glyph_px(gp));
    }
    w = 0;
    for (int p = a; p < b;) {
      int gp = p;
      p += utf8_decode(&t[p], &t[re], &cp);
      w += body_cp_adv(cp, glyph_px(gp));
    }
    if (w < 1)
      w = 1;
  } else if (text_rows == 0 && img_w > 0) { /* image-only box */
    w = img_w;
  } else { /* block / multi-line: content col */
    set_block(B->line_off[first]);
    x = B->line_x[first] + cur_bindent * INDENT_PX;
    w = B->line_w[first] - cur_bindent * INDENT_PX;
    if (w < 1)
      w = 1;
  }
  browser_buf_unlock();
  *ox = x;
  *oy = y;
  *ow = w;
  *oh = h;
  return 1;
}

int browser_scroll_px(void) { return B->pg->scroll; } /* window.scrollY (document px) */

void browser_viewport_px(int *w, int *h)
{
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  if (w) {
    int vw = BW - SCROLL_W;
    *w = vw > 0 ? vw : 0;
  }
  if (h)
    *h = phh;
}

/* The CSS box layer. Behind a flag so a regression is one flip back to the pure
 * flat text renderer. The box tree (layout.c) supplies each block's content
 * origin/width; inline text still flows through the shared wrap below, and
 * vertical spacing is quantised to text rows (Stage 6 moves to pixel
 * scrolling). */
static int g_box_layout = 1;

static int rows_for(int px) { return px > 0 ? (px + LINE_H - 1) / LINE_H : 0; }
static int max0(int v) { return v > 0 ? v : 0; }
/* Reserve `n` LINE_H-tall blank lines (flex bands still address rows via
 * line_y). */
static void add_n_blanks(int n, int off, int x, int w)
{
  while (n-- > 0)
    add_line(off, 0, x, w, LINE_H);
}
/* Reserve one blank line of exactly `h` px — exact margin/padding spacing, no
 * rounding to whole rows (the Stage 6 phase 2 improvement). No-op for h<=0. */
static void add_blank_px(int h, int off, int x, int w)
{
  if (h > 0)
    add_line(off, 0, x, w, h);
}
/* Line height for the block set by set_block — LINE_H today (body text +
 * headings, which are capped at LINE_H); Stage 6 phase 3 derives a taller
 * height from a larger CSS font-size, which is what lets big headings exceed
 * one row. */
static int block_line_h(void)
{
  if (cur_blh > 0)
    return cur_blh < 6 ? 6 : cur_blh; /* explicit CSS line-height */
  if (cur_bpx <= 0)
    return LINE_H;                      /* default body font */
  int lh = cur_bpx + (cur_bpx + 2) / 4; /* ~1.25 line-height */
  return lh < LINE_H ? LINE_H : lh;
}

/* Wrap the glyph slice [t0,t1) at content origin `base_x` px / width `w` px,
 * appending display lines (and reserving inline-image rows). The per-glyph bp
 * byte still governs heading size / monospace / centring / list indent within
 * the line; the box tree supplies base_x/w. Shared by the flat renderer (the
 * whole stream) and the box layout (one slice per block). */
static void flow_slice(int t0, int t1, int base_x, int w)
{
  /* Text/attr buffers are (re)built together by the layout walk; a relayout
   * triggered mid-flight (e.g. by a script mutating the DOM) can reach here
   * before they exist. Never scan a NULL buffer, and clamp to what's built. */
  if (!B->pg->text || !B->pg->attr)
    return;
  if (t1 > B->pg->text_len)
    t1 = B->pg->text_len;
  if (w < 8)
    w = 8;
  int i = t0;
  while (i < t1) {
    int j = i;
    while (j < t1 && B->pg->text[j] != '\n')
      j++;
    set_block(i);
    int avail = w - cur_bindent * INDENT_PX;
    if (avail < 8)
      avail = 8;
    if (j == i) { /* blank line / replaced-element anchor */
      int img = -1;
      struct media_el *me;
      for (int q = 0; q < B->pg->n_imgs; q++)
        if (B->pg->imgs[q].text_pos == i) {
          img = q;
          break;
        }
      if (img >= 0) {
        struct inl_img *im = &B->pg->imgs[img];
        int dw = 0, dh = 0;
        img_disp_size(im, w, &dw, &dh);
        im->rows = 1; /* one display line, exact px tall */
        im->start_row = B->nlines;
        add_line(i, 0, base_x, w, dh > 0 ? dh : LINE_H);
      } else if ((me = media_at_text_pos(i)) != 0) {
        int mr = media_widget_rows(me, w, LINE_H);
        media_set_layout(me, B->nlines, mr);
        for (int r = 0; r < mr; r++)
          add_line(i, 0, base_x, w, LINE_H);
      } else {
        add_line(i, 0, base_x, w, block_line_h());
      }
    } else {
      int ls = i;
      while (ls < j) {
        int brk;
        if (B->pg->attr[ls] & ST_NOWRAP) {
          brk = j; /* white-space:nowrap — one line, may overflow */
        } else {
          /* greedy fit by pixel width; break at the last space if a word
           * would overflow, else hard-break (one char always fits). */
          int wd = 0, e = ls, lastsp = -1;
          while (e < j) {
            unsigned cp;
            int nb = utf8_decode(&B->pg->text[e], &B->pg->text[j], &cp);
            int cw = body_cp_adv(cp, glyph_px(e));
            if (wd + cw > avail && e > ls)
              break;
            if (cp == ' ')
              lastsp = e;
            wd += cw;
            e += nb;
          }
          brk = (e < j && lastsp > ls) ? lastsp : e;
        }
        add_line(ls, brk - ls, base_x, w, line_height_for(ls, brk));
        ls = brk;
        while (ls < j && B->pg->text[ls] == ' ')
          ls++;
      }
    }
    i = j + 1;
  }
}

/* Resolve a CSS length to px: an explicit px wins, else a percent of `basis`,
 * else -1 (auto/unset). Used for width/height and their min/max against the
 * container. */
static int resolve_len(int px, int pct, int basis)
{
  if (px >= 0)
    return px;
  if (pct >= 0)
    return (int)((long)basis * pct / 100);
  return -1;
}

/* Resolve a box's border-box width within `avail_w` (the containing block's
 * content width). `frame` is the horizontal padding+border. With
 * box-sizing:content-box (the default) CSS width/min/max size the CONTENT box,
 * so `frame` is added back; with border-box they size the border box directly.
 * `auto` width fills what the margins leave. min-/max-width clamp, then the
 * result is kept within the containing block. */
static int box_width(const struct lbox *b, int avail_w, int frame, int ml, int mr)
{
  int extra = b->border_box ? 0 : frame; /* added to a content-box length */
  int spec = resolve_len(b->spec_w, b->pct_w, avail_w);
  int box_w = spec >= 0 ? spec + extra : avail_w - ml - mr;

  int maxw = resolve_len(b->max_w, b->pct_maxw, avail_w);
  if (maxw >= 0 && box_w > maxw + extra)
    box_w = maxw + extra;
  int minw = resolve_len(b->min_w, b->pct_minw, avail_w);
  if (minw >= 0 && box_w < minw + extra)
    box_w = minw + extra;

  if (box_w < frame + 1)
    box_w = frame + 1; /* room for >=1px of content */
  if (box_w < 16)
    box_w = 16;
  if (box_w > avail_w)
    box_w = avail_w;
  return box_w;
}

/* Minimum content-box height in px, or -1 if the box is purely content-sized.
 * CSS height is treated as a floor (short content is padded so the background
 * fills, but taller content overflows — we don't clip); min-height raises the
 * floor and max-height caps it. Percent heights need a definite container,
 * which this content- driven flow doesn't have, so they're ignored.
 * box-sizing:border-box values include the vertical frame, removed here to get
 * the content height. */
static int box_min_content_h(const struct lbox *b, int vframe)
{
  int frame = b->border_box ? vframe : 0;
  int h = b->spec_h >= 0 ? b->spec_h - frame : -1;
  int minh = b->min_h >= 0 ? b->min_h - frame : -1;
  int target = h > minh ? h : minh;
  if (b->max_h >= 0) {
    int maxh = b->max_h - frame;
    if (maxh < 0)
      maxh = 0;
    if (target < 0 || target > maxh)
      target = maxh; /* max-height also caps */
  }
  return target;
}

/* --- flex (flex-direction:row)
 * ------------------------------------------------ * Row flex items must sit
 * side by side on the same vertical band, which the one- slice-per-row line
 * table can't express. So a row flex container reserves a blank band in the
 * main table for height/scroll, and each item's wrapped text is stashed in the
 * overlay (struct fslice) at its own column x, drawn over the band. Item box
 * geometry is set here so paint_boxes still draws each item's
 * background/border. Item content is treated as inline text (its full glyph
 * slice); nested block boxes inside an item don't get their own side-by-side
 * geometry — a documented limit of the band overlay (the common
 * navbar/button-row/card-row case works). */

/* Natural (max-content) width of a glyph slice in px: the widest unwrapped
 * line. */
static int slice_maxcontent(int t0, int t1)
{
  int best = 0, i = t0;
  while (i <= t1) {
    int j = i;
    while (j < t1 && B->pg->text[j] != '\n')
      j++;
    if (j > i) {
      set_block(i);
      int w = cur_bindent * INDENT_PX;
      unsigned cp;
      for (int p = i; p < j;) {
        int gp = p;
        p += utf8_decode(&B->pg->text[p], &B->pg->text[j], &cp);
        w += body_cp_adv(cp, glyph_px(gp));
      }
      if (w > best)
        best = w;
    }
    if (j >= t1)
      break;
    i = j + 1;
  }
  return best;
}

/* A flex item's flex-basis as a border-box main size: flex-basis, else width,
 * else its max-content text width. content-box lengths get the horizontal frame
 * added. */
static int flex_base_main(const struct lbox *it, int cont_w)
{
  int frame = it->border.l + it->border.r + it->padding.l + it->padding.r;
  int spec = -1;
  if (it->flex_basis >= 0 || it->pct_basis >= 0)
    spec = resolve_len(it->flex_basis, it->pct_basis, cont_w);
  else if (it->spec_w >= 0 || it->pct_w >= 0)
    spec = resolve_len(it->spec_w, it->pct_w, cont_w);
  if (spec >= 0)
    return spec + (it->border_box ? 0 : frame);
  return slice_maxcontent(it->t0, it->t1) + frame;
}

/* Clamp a resolved border-box main size to the item's min/max-width and a 1px
 * floor. */
static int flex_clamp_main(const struct lbox *it, int v, int cont_w)
{
  int frame = it->border.l + it->border.r + it->padding.l + it->padding.r;
  int extra = it->border_box ? 0 : frame;
  int maxw = resolve_len(it->max_w, it->pct_maxw, cont_w);
  if (maxw >= 0 && v > maxw + extra)
    v = maxw + extra;
  int minw = resolve_len(it->min_w, it->pct_minw, cont_w);
  if (minw >= 0 && v < minw + extra)
    v = minw + extra;
  if (v < frame + 1)
    v = frame + 1;
  return v;
}

#define FLEX_MAX_ITEMS 64

/* Lay out a flex-direction:row container's direct children side by side within
 * the container content box [content_x, content_x+content_w). Appends the band
 * to the main table and the items' text to the overlay; sets each item's box
 * geometry. */
static void flow_flex_row(struct lbox *bx, int idx, int content_x, int content_w)
{
  struct lbox *cont = &bx[idx];
  int items[FLEX_MAX_ITEMS], nit = 0;
  for (int j = idx + 1; j < cont->end && nit < FLEX_MAX_ITEMS;) {
    if (bx[j].parent == idx) {
      items[nit++] = j;
      j = bx[j].end;
    } else
      j++;
  }
  if (nit == 0)
    return;

  int gap = cont->gap_col;
  int total_gap = gap * (nit - 1);

  int base[FLEX_MAX_ITEMS], main_sz[FLEX_MAX_ITEMS];
  long sumbase = 0, sumgrow = 0, sumshrinkw = 0;
  for (int k = 0; k < nit; k++) {
    base[k] = flex_base_main(&bx[items[k]], content_w);
    main_sz[k] = base[k];
    sumbase += base[k];
    int g = bx[items[k]].flex_grow;
    if (g < 0)
      g = 0;
    int s = bx[items[k]].flex_shrink;
    if (s < 0)
      s = 1;
    sumgrow += g;
    sumshrinkw += (long)s * base[k];
  }
  long avail = content_w - total_gap;
  long freepx = avail - sumbase;
  if (freepx > 0 && sumgrow > 0) {
    for (int k = 0; k < nit; k++) {
      int g = bx[items[k]].flex_grow;
      if (g < 0)
        g = 0;
      main_sz[k] = base[k] + (int)(freepx * g / sumgrow);
    }
  } else if (freepx < 0 && sumshrinkw > 0) {
    long need = -freepx;
    for (int k = 0; k < nit; k++) {
      int s = bx[items[k]].flex_shrink;
      if (s < 0)
        s = 1;
      main_sz[k] = base[k] - (int)(need * ((long)s * base[k]) / sumshrinkw);
    }
  }
  long used = 0;
  for (int k = 0; k < nit; k++) {
    main_sz[k] = flex_clamp_main(&bx[items[k]], main_sz[k], content_w);
    used += main_sz[k];
  }

  int leftover = (int)(content_w - total_gap - used);
  if (leftover < 0)
    leftover = 0;
  int start = 0, between = 0;
  signed char jc = cont->justify < 0 ? CSS_JUST_START : cont->justify;
  switch (jc) {
  case CSS_JUST_CENTER:
    start = leftover / 2;
    break;
  case CSS_JUST_END:
    start = leftover;
    break;
  case CSS_JUST_BETWEEN:
    if (nit > 1)
      between = leftover / (nit - 1);
    else
      start = leftover / 2;
    break;
  case CSS_JUST_AROUND:
    between = leftover / nit;
    start = between / 2;
    break;
  case CSS_JUST_EVENLY:
    between = leftover / (nit + 1);
    start = between;
    break;
  default:
    break; /* flex-start */
  }

  /* Phase 1: flow each item's text (top-aligned for now) to learn its cross
   * size. An item's outer (border-box) height is its content rows clamped by
   * height/ min-height plus its vertical frame; the band is the tallest item,
   * raised to the container's own content-height floor so align-items has room
   * to work. */
  int band_top = B->nlines;
  int band_top_y = doc_px(); /* pixel top of the band (its rows are added below) */
  int rows_of[FLEX_MAX_ITEMS], ov_start[FLEX_MAX_ITEMS], item_x[FLEX_MAX_ITEMS], toppad[FLEX_MAX_ITEMS];
  int x = content_x + start, band_h = 0;
  for (int k = 0; k < nit; k++) {
    struct lbox *it = &bx[items[k]];
    int fl = it->border.l + it->padding.l, fr = it->border.r + it->padding.r;
    int cw = main_sz[k] - fl - fr;
    if (cw < 4)
      cw = 4;
    item_x[k] = x;
    ov_start[k] = B->n_fslices;
    g_ov_active = 1;
    g_ov_base = band_top;
    g_ov_n = 0;
    flow_slice(it->t0, it->t1, x + fl, cw);
    g_ov_active = 0;
    int vframe = it->border.t + it->border.b + it->padding.t + it->padding.b;
    int content_px = g_ov_n * LINE_H;
    int minch = box_min_content_h(it, vframe);
    if (minch > content_px)
      content_px = minch;
    int orows = rows_for(content_px + vframe);
    if (orows < 1)
      orows = 1;
    rows_of[k] = orows;
    toppad[k] = rows_for(it->border.t + it->padding.t);
    if (orows > band_h)
      band_h = orows;
    x += main_sz[k] + gap + between;
  }
  int cvframe = cont->border.t + cont->border.b + cont->padding.t + cont->padding.b;
  int cminch = box_min_content_h(cont, cvframe);
  if (cminch > 0 && rows_for(cminch) > band_h)
    band_h = rows_for(cminch);
  if (band_h < 1)
    band_h = 1;

  /* Phase 2: cross-axis place each item (align-items), inset its text by the
   * item's top padding, and record geometry so paint_boxes draws its
   * background/border. */
  signed char ai = cont->align_items < 0 ? CSS_ALIGN_STRETCH : cont->align_items;
  for (int k = 0; k < nit; k++) {
    struct lbox *it = &bx[items[k]];
    int h_rows = (ai == CSS_ALIGN_STRETCH) ? band_h : rows_of[k];
    int off_rows = 0;
    if (ai == CSS_ALIGN_CENTER)
      off_rows = (band_h - h_rows) / 2;
    else if (ai == CSS_ALIGN_END)
      off_rows = band_h - h_rows;
    int shift = off_rows + toppad[k];
    if (shift) {
      int end = (k + 1 < nit) ? ov_start[k + 1] : B->n_fslices;
      for (int s = ov_start[k]; s < end; s++)
        B->fslices[s].row += shift;
    }
    int fl = it->border.l + it->padding.l, fr = it->border.r + it->padding.r;
    int ft = it->border.t + it->padding.t;
    int vframe = ft + it->border.b + it->padding.b;
    it->x = item_x[k];
    it->w = main_sz[k];
    it->y = band_top_y + off_rows * LINE_H;
    it->h = h_rows * LINE_H;
    it->cx = item_x[k] + fl;
    it->cw = main_sz[k] - fl - fr;
    if (it->cw < 4)
      it->cw = 4;
    it->cy = it->y + ft;
    it->ch = it->h - vframe;
    if (it->ch < 0)
      it->ch = 0;
  }

  add_n_blanks(band_h, cont->t0, content_x, content_w);
}

/* --- grid (display:grid)
 * ------------------------------------------------------ * Like flex-row, a
 * grid needs items placed at arbitrary columns within a band the row-quantised
 * line table can't express, so it reuses the same band overlay: the container
 * reserves a blank band (keeping scroll/height row-indexed) and each cell's
 * wrapped text is stashed in the overlay at the cell's column x, drawn over the
 * band. Column tracks are sized (px / % / fr / auto→max-content) within the
 * content box; items auto-flow row-by-row honouring grid-column/row spans; each
 * grid row's height is its tallest cell (row-quantised). Item box geometry is
 * set so paint_boxes draws each cell's background/border. Cells stretch to fill
 * their track span by default (align-items/justify-items:stretch). Limits:
 * dense packing of row-spanning items isn't tracked (a cell a rowspan reserves
 * may be reused by the next row); a cell's inline text is its own glyph slice —
 * nested block boxes inside a cell don't get their own side-by-side geometry
 * (same limit as flex). */
#define GRID_MAX_ITEMS 128
#define GRID_MAX_ROWS 64

static void flow_grid(struct lbox *bx, int idx, int content_x, int content_w)
{
  struct lbox *cont = &bx[idx];
  int ncols = cont->n_cols > 0 ? cont->n_cols : 1;
  if (ncols > CSS_GRID_MAX)
    ncols = CSS_GRID_MAX;

  int items[GRID_MAX_ITEMS], nit = 0;
  for (int j = idx + 1; j < cont->end && nit < GRID_MAX_ITEMS;) {
    if (bx[j].parent == idx) {
      items[nit++] = j;
      j = bx[j].end;
    } else
      j++;
  }
  if (nit == 0)
    return;

  int gcol = cont->gap_col > 0 ? cont->gap_col : 0;
  int grow_gap = cont->gap_row > 0 ? cont->gap_row : 0;

  /* --- auto-placement: assign each item a (col,row) + span
   * (grid-auto-flow:row). A span wider than the remaining columns wraps to the
   * next row. Row spans are honoured for height/geometry but not tracked as
   * occupied cells (v1 limit). */
  int it_col[GRID_MAX_ITEMS], it_row[GRID_MAX_ITEMS], it_cs[GRID_MAX_ITEMS], it_rs[GRID_MAX_ITEMS];
  int cur_c = 0, cur_r = 0, nrows = 0;
  for (int k = 0; k < nit; k++) {
    struct lbox *it = &bx[items[k]];
    int cs = it->col_span > 0 ? it->col_span : 1;
    if (cs > ncols)
      cs = ncols;
    int rs = it->row_span > 0 ? it->row_span : 1;
    if (cur_c + cs > ncols) {
      cur_c = 0;
      cur_r++;
    }
    it_col[k] = cur_c;
    it_row[k] = cur_r;
    it_cs[k] = cs;
    it_rs[k] = rs;
    if (cur_r + rs > nrows)
      nrows = cur_r + rs;
    cur_c += cs;
    if (cur_c >= ncols) {
      cur_c = 0;
      cur_r++;
    }
  }
  if (nrows < 1)
    nrows = 1;
  if (nrows > GRID_MAX_ROWS)
    nrows = GRID_MAX_ROWS;

  /* --- column track sizing: resolve fixed (px/%) tracks and auto tracks
   * (max-content of their single-column items), then share the leftover across
   * fr tracks (or grow auto tracks if there are none), and scale down on
   * overflow. */
  int colw[CSS_GRID_MAX], is_fr[CSS_GRID_MAX], fr_w[CSS_GRID_MAX], is_auto[CSS_GRID_MAX];
  int total_gap = gcol * (ncols - 1);
  int avail = content_w - total_gap;
  if (avail < ncols)
    avail = ncols;
  long fr_sum = 0, fixed = 0;
  int auto_cnt = 0;
  for (int c = 0; c < ncols; c++) {
    struct css_track t = cont->n_cols > 0 ? cont->cols_t[c] : (struct css_track){CSS_TRK_FR, 1};
    is_fr[c] = is_auto[c] = 0;
    fr_w[c] = 0;
    colw[c] = 0;
    if (t.kind == CSS_TRK_PX) {
      colw[c] = t.v > 0 ? t.v : 0;
      fixed += colw[c];
    } else if (t.kind == CSS_TRK_PCT) {
      colw[c] = (int)((long)content_w * t.v / 100);
      if (colw[c] < 0)
        colw[c] = 0;
      fixed += colw[c];
    } else if (t.kind == CSS_TRK_FR) {
      is_fr[c] = 1;
      fr_w[c] = t.v > 0 ? t.v : 1;
      fr_sum += fr_w[c];
    } else {
      is_auto[c] = 1;
      auto_cnt++;
    }
  }
  for (int c = 0; c < ncols; c++) {
    if (!is_auto[c])
      continue;
    int best = 0;
    for (int k = 0; k < nit; k++) {
      if (it_cs[k] != 1 || it_col[k] != c)
        continue;
      struct lbox *it = &bx[items[k]];
      int frame = it->border.l + it->border.r + it->padding.l + it->padding.r;
      int mc = slice_maxcontent(it->t0, it->t1) + frame;
      if (mc > best)
        best = mc;
    }
    colw[c] = best;
    fixed += best;
  }
  long leftover = avail - fixed;
  if (leftover < 0)
    leftover = 0;
  if (fr_sum > 0) {
    for (int c = 0; c < ncols; c++)
      if (is_fr[c])
        colw[c] = (int)(leftover * fr_w[c] / fr_sum);
  } else if (leftover > 0 && auto_cnt > 0) {
    int add = (int)(leftover / auto_cnt);
    for (int c = 0; c < ncols; c++)
      if (is_auto[c])
        colw[c] += add;
  }
  long tot = 0;
  for (int c = 0; c < ncols; c++) {
    if (colw[c] < 0)
      colw[c] = 0;
    tot += colw[c];
  }
  if (tot > avail && tot > 0)
    for (int c = 0; c < ncols; c++)
      colw[c] = (int)((long)colw[c] * avail / tot);
  for (int c = 0; c < ncols; c++)
    if (colw[c] < 1)
      colw[c] = 1;

  int track_x[CSS_GRID_MAX + 1];
  track_x[0] = content_x;
  for (int c = 0; c < ncols; c++)
    track_x[c + 1] = track_x[c] + colw[c] + gcol;

  /* --- flow each item into the overlay (top of band for now), measuring its
   * content rows, its column x and its border-box main width. */
  int band_top = B->nlines;
  int band_top_y = doc_px();
  int rows_of[GRID_MAX_ITEMS], ov_start[GRID_MAX_ITEMS], toppad[GRID_MAX_ITEMS];
  int it_x[GRID_MAX_ITEMS], it_w[GRID_MAX_ITEMS];
  for (int k = 0; k < nit; k++) {
    struct lbox *it = &bx[items[k]];
    int cs = it_cs[k];
    int iw = 0;
    for (int c = it_col[k]; c < it_col[k] + cs && c < ncols; c++)
      iw += colw[c];
    iw += gcol * (cs - 1);
    it_x[k] = track_x[it_col[k]];
    it_w[k] = iw;
    int fl = it->border.l + it->padding.l, fr = it->border.r + it->padding.r;
    int cw = iw - fl - fr;
    if (cw < 4)
      cw = 4;
    ov_start[k] = B->n_fslices;
    g_ov_active = 1;
    g_ov_base = band_top;
    g_ov_n = 0;
    flow_slice(it->t0, it->t1, it_x[k] + fl, cw);
    g_ov_active = 0;
    int vframe = it->border.t + it->border.b + it->padding.t + it->padding.b;
    int content_px = g_ov_n * LINE_H;
    int minch = box_min_content_h(it, vframe);
    if (minch > content_px)
      content_px = minch;
    int orows = rows_for(content_px + vframe);
    if (orows < 1)
      orows = 1;
    rows_of[k] = orows;
    toppad[k] = rows_for(it->border.t + it->padding.t);
  }

  /* --- row-track heights (rows): a px grid-template-rows entry is a floor; a
   * single-row item raises its row; a row-spanning item extends its last row.
   */
  int rowh[GRID_MAX_ROWS];
  for (int r = 0; r < nrows; r++) {
    rowh[r] = 0;
    if (cont->n_rows > 0 && r < cont->n_rows && cont->rows_t[r].kind == CSS_TRK_PX) {
      int rr = rows_for(cont->rows_t[r].v);
      if (rr > rowh[r])
        rowh[r] = rr;
    }
  }
  for (int k = 0; k < nit; k++)
    if (it_rs[k] == 1 && it_row[k] < nrows && rows_of[k] > rowh[it_row[k]])
      rowh[it_row[k]] = rows_of[k];
  for (int r = 0; r < nrows; r++)
    if (rowh[r] < 1)
      rowh[r] = 1;

  int gap_rows = rows_for(grow_gap);
  int row_off[GRID_MAX_ROWS + 1];
  row_off[0] = 0;
  for (int r = 0; r < nrows; r++)
    row_off[r + 1] = row_off[r] + rowh[r] + (r + 1 < nrows ? gap_rows : 0);
  for (int k = 0; k < nit; k++) { /* extend last spanned row for tall row-span items */
    if (it_rs[k] <= 1)
      continue;
    int r0 = it_row[k], r1 = r0 + it_rs[k];
    if (r1 > nrows)
      r1 = nrows;
    int have = row_off[r1] - row_off[r0] - (r1 < nrows ? gap_rows : 0);
    if (rows_of[k] > have && r1 - 1 >= 0 && r1 - 1 < nrows) {
      rowh[r1 - 1] += rows_of[k] - have;
      for (int r = r1 - 1; r < nrows; r++)
        row_off[r + 1] = row_off[r] + rowh[r] + (r + 1 < nrows ? gap_rows : 0);
    }
  }
  int band_h = row_off[nrows];
  int cvframe = cont->border.t + cont->border.b + cont->padding.t + cont->padding.b;
  int cminch = box_min_content_h(cont, cvframe);
  if (cminch > 0 && rows_for(cminch) > band_h)
    band_h = rows_for(cminch);
  if (band_h < 1)
    band_h = 1;

  /* --- place: shift each cell's overlay text to its row (align-items on the
   * block axis) + top padding, and record geometry so paint_boxes draws its
   * bg/border. */
  signed char ai = cont->align_items < 0 ? CSS_ALIGN_STRETCH : cont->align_items;
  for (int k = 0; k < nit; k++) {
    struct lbox *it = &bx[items[k]];
    int r0 = it_row[k], r1 = r0 + it_rs[k];
    if (r1 > nrows)
      r1 = nrows;
    int cell_rows = row_off[r1] - row_off[r0] - (r1 < nrows ? gap_rows : 0);
    if (cell_rows < 1)
      cell_rows = 1;
    int h_rows = (ai == CSS_ALIGN_STRETCH) ? cell_rows : rows_of[k];
    if (h_rows > cell_rows)
      h_rows = cell_rows;
    int off_rows = 0;
    if (ai == CSS_ALIGN_CENTER)
      off_rows = (cell_rows - h_rows) / 2;
    else if (ai == CSS_ALIGN_END)
      off_rows = cell_rows - h_rows;
    int base_row = row_off[r0] + off_rows;
    int shift = base_row + toppad[k];
    if (shift) {
      int end = (k + 1 < nit) ? ov_start[k + 1] : B->n_fslices;
      for (int s = ov_start[k]; s < end; s++)
        B->fslices[s].row += shift;
    }
    int fl = it->border.l + it->padding.l, fr = it->border.r + it->padding.r;
    int ft = it->border.t + it->padding.t;
    int vframe = ft + it->border.b + it->padding.b;
    it->x = it_x[k];
    it->w = it_w[k];
    it->y = band_top_y + base_row * LINE_H;
    it->h = h_rows * LINE_H;
    it->cx = it_x[k] + fl;
    it->cw = it_w[k] - fl - fr;
    if (it->cw < 4)
      it->cw = 4;
    it->cy = it->y + ft;
    it->ch = it->h - vframe;
    if (it->ch < 0)
      it->ch = 0;
  }

  add_n_blanks(band_h, cont->t0, content_x, content_w);
}

/* Lay out one box: blank rows for top margin/border/padding, then its own
 * inline text interleaved with child boxes (each recursively laid out), then
 * bottom spacing. Geometry is stored in document px; vertical position is the
 * row cursor (B->nlines) * LINE_H. `x`/`avail_w` are the containing block's
 * content origin and width. The box honours CSS width with min-/max-width and
 * box-sizing, `height`/ min-height as a floor (short content is padded so the
 * background fills the box, capped by max-height), `margin:auto` (horizontal
 * centring) and `position` offsets. Because rows are sequential, a vertical
 * offset (`top`/`bottom`) moves only the painted box; horizontal offsets move
 * box+text. */
static void flow_box(struct lbox *bx, int idx, int x, int avail_w)
{
  struct lbox *b = &bx[idx];
  int ml = max0(b->margin.l), mr = max0(b->margin.r);
  int frame = b->border.l + b->border.r + b->padding.l + b->padding.r;
  int box_w = box_width(b, avail_w, frame, ml, mr);

  /* Horizontal placement: auto margins absorb the slack (CSS block centring).
   */
  int slack = avail_w - box_w - ml - mr;
  int box_x;
  if (b->ml_auto && b->mr_auto)
    box_x = x + ml + (slack > 0 ? slack / 2 : 0);
  else if (b->ml_auto)
    box_x = x + ml + (slack > 0 ? slack : 0);
  else
    box_x = x + ml;

  /* position: relative shifts from the flow spot; absolute/fixed are placed
   * from the containing block's edges. Horizontal offset moves the text with
   * the box; vertical offset is applied to the painted rect only (dy). */
  int dy = 0;
  if (b->pos == CSS_POS_REL || b->pos == CSS_POS_STICKY) {
    if (b->off_l >= 0)
      box_x += b->off_l;
    else if (b->off_r >= 0)
      box_x -= b->off_r;
    if (b->off_t >= 0)
      dy = b->off_t;
    else if (b->off_b >= 0)
      dy = -b->off_b;
  } else if (b->pos == CSS_POS_ABS || b->pos == CSS_POS_FIXED) {
    if (b->off_l >= 0)
      box_x = x + b->off_l;
    else if (b->off_r >= 0)
      box_x = x + avail_w - box_w - b->off_r;
    if (b->off_t >= 0)
      dy = b->off_t;
    else if (b->off_b >= 0)
      dy = -b->off_b;
  }

  int content_x = box_x + b->border.l + b->padding.l;
  int content_w = box_w - frame;
  if (content_w < 8)
    content_w = 8;

  add_blank_px(max0(b->margin.t), b->t0, box_x, box_w);
  int box_top_y = doc_px();
  add_blank_px(b->border.t + b->padding.t, b->t0, content_x, content_w);
  int content_top_y = doc_px();

  b->x = box_x;
  b->w = box_w;
  b->cx = content_x;
  b->cw = content_w;

  if (b->kind == LB_GRID) {
    /* grid: children placed into a 2-D track band via the overlay. */
    flow_grid(bx, idx, content_x, content_w);
  } else if (b->kind == LB_FLEX && b->flex_dir == 0) {
    /* row flex: children laid side by side via the band overlay (column flex
     * keeps the block-stacking path below — vertical stacking is already
     * right). */
    flow_flex_row(bx, idx, content_x, content_w);
  } else {
    int pos = b->t0;
    for (int j = idx + 1; j < b->end;) {
      if (bx[j].parent != idx) {
        j++;
        continue;
      } /* not a direct child */
      flow_slice(pos, bx[j].t0, content_x, content_w); /* inline text before it */
      flow_box(bx, j, content_x, content_w);
      pos = bx[j].t1;
      j = bx[j].end; /* skip its descendants */
    }
    flow_slice(pos, b->t1, content_x, content_w);
  }

  /* Pad short content up to the box's minimum content height
   * (height/min-height, capped by max-height) so the background fills; taller
   * content overflows. */
  int vframe = b->border.t + b->border.b + b->padding.t + b->padding.b;
  int min_ch = box_min_content_h(b, vframe);
  if (min_ch > 0) {
    int have = doc_px() - content_top_y;
    if (min_ch > have)
      add_blank_px(min_ch - have, b->t1, content_x, content_w);
  }

  int content_bot_y = doc_px();
  add_blank_px(b->padding.b + b->border.b, b->t1, content_x, content_w);
  int box_bot_y = doc_px();
  add_blank_px(max0(b->margin.b), b->t1, box_x, box_w);

  b->y = box_top_y + dy;
  b->h = box_bot_y - box_top_y;
  b->cy = content_top_y + dy;
  b->ch = content_bot_y - content_top_y;
}

/* Lay out the whole box tree (top-level boxes stacked, with any stray text
 * outside them flowed full width). Appends to the line table; sets every box's
 * geometry. */
static void layout_flow(int paw)
{
  int n;
  struct lbox *bx = layout_pool(&n);
  int pos = 0;
  for (int j = 0; j < n;) {
    if (bx[j].parent != -1) {
      j++;
      continue;
    }
    flow_slice(pos, bx[j].t0, 4, paw);
    flow_box(bx, j, 4, paw);
    pos = bx[j].t1;
    j = bx[j].end;
  }
  flow_slice(pos, B->pg->text_len, 4, paw);
}

/* Word-wrap B->pg->text into display lines that fit `paw` pixels wide
 * (proportional font), reserving rows for inline images. wrap_cols caches the
 * pane width. */
static void rewrap(int paw)
{
  if (paw < 8)
    paw = 8;
  /* Single consistency guard for the whole layout+paint below: the worker
   * rebuilds text/attr/box-tree during a (re)layout, and although render()
   * holds browser_buf_lock (so nothing changes mid-frame), it can still observe the
   * instant where the buffers aren't built yet. Don't lay out or paint over a
   * NULL text buffer — drop to an empty page this frame and force a re-wrap
   * once it's ready. This replaces per-reader NULL checks across every
   * flow/measure fn. */
  if (!B->pg->text) {
    B->nlines = 0;
    B->layout_dirty = 1;
    return;
  }
  if (!B->layout_dirty && B->wrap_cols == paw)
    return;
  for (int q = 0; q < B->pg->n_imgs; q++)
    B->pg->imgs[q].start_row = -1;
  media_reset_layout();
  B->nlines = 0;
  B->n_fslices = 0;
  int n;
  layout_pool(&n);
  if (g_box_layout && n > 0)
    layout_flow(paw); /* box-driven layout */
  else
    flow_slice(0, B->pg->text_len, 4, paw); /* flat flow  */
  B->layout_dirty = 0;
  B->wrap_cols = paw;
  B->layout_seq++; /* geometry changed: devtools box model + highlight */
  int max = scroll_max();
  if (B->pg->scroll > max)
    B->pg->scroll = max;
  if (B->pg->scroll < 0)
    B->pg->scroll = 0;
}

/* ------------------------------------------------------------- image draw */

/* PNG byte source: pulls straight out of the already-fetched body buffer, so
 * the decoder streams from B->pg->raw without a second copy. */
struct rawrd {
  int pos;
};
static int rawrd_get(void *c)
{
  struct rawrd *r = c;
  if (r->pos >= B->pg->raw_len)
    return -1;
  return (unsigned char)B->pg->raw[r->pos++];
}

/* Decode state: a fit-to-pane letterbox, blitting each PNG scanline directly
 * into the window buffer inside the content pane (mirrors imgview's v_row). */
struct imgblit {
  int px, py;              /* pane top-left (window coords)            */
  unsigned iw, ih;         /* source image size                       */
  unsigned dw, dh, ox, oy; /* drawn size + offset within the pane      */
  unsigned next_r;         /* next dest row awaiting its source row    */
};

/* Composite one RGBA source pixel over an existing framebuffer pixel. */
static unsigned blend_px(const unsigned char *p, unsigned dst)
{
  unsigned a = p[3];
  if (a == 255)
    return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
  if (a == 0)
    return dst;
  unsigned ia = 255 - a;
  unsigned r = (p[0] * a + ((dst >> 16) & 0xff) * ia) / 255;
  unsigned g = (p[1] * a + ((dst >> 8) & 0xff) * ia) / 255;
  unsigned b = (p[2] * a + (dst & 0xff) * ia) / 255;
  return (r << 16) | (g << 8) | b;
}

static int img_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  (void)w;
  struct imgblit *v = c;
  unsigned *fb = browser_surface();
  while (v->next_r < v->dh && (v->next_r * v->ih / v->dh) == y) {
    unsigned *drow = fb + (unsigned long)(v->py + v->oy + v->next_r) * BW + v->px + v->ox;
    for (unsigned x = 0; x < v->dw; x++) {
      const unsigned char *p = rgba + (x * v->iw / v->dw) * 4;
      drow[x] = blend_px(p, drow[x]);
    }
    v->next_r++;
  }
  return 0;
}

/* Row-copy sink: write each decoded PNG scanline into a full RGBA buffer.
 * Shared by the inline-image cache (imgc_get) and the full-page image cache
 * below. */
struct imgc_fill {
  unsigned char *buf;
  unsigned iw, ih;
};
static int imgc_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  struct imgc_fill *d = c;
  if (y < d->ih) {
    unsigned n = (w < d->iw ? w : d->iw) * 4;
    memcpy(d->buf + (unsigned long)y * d->iw * 4, rgba, n);
  }
  return 0;
}

/* Decoded full-page image cache (MODE_IMAGE): the image page would otherwise
 * re-inflate the whole PNG on every repaint (hover, resize, scrollbar). Decode
 * once into RGBA, keyed by the page + its raw buffer, then scale-blit
 * thereafter. Images past DIMG_MAX_PX aren't held — they stream each frame.
 * Cleared on a page change (imgc_clear) or when the raw buffer changes.
 * Main-thread-only. */
#define DIMG_MAX_PX (16u * 1024 * 1024)
static struct {
  struct page *pg;
  const char *src;
  int src_len;
  unsigned iw, ih;
  unsigned char *rgba;
} g_dimg;

/* Streaming decode straight to the pane (no held copy): the cache's fallback
 * for an image too large to buffer, and the unreadable-image error path. */
static void draw_image_stream(int px, int py, int pw, int ph)
{
  struct rawrd r = {0};
  unsigned iw = 0, ih = 0;
  if (upng_dimensions(rawrd_get, &r, &iw, &ih) != 0 || !iw || !ih) {
    gfx_ui_text(px + 8, py + 8, "Cannot decode image (unreadable or oversized interlaced PNG).", 0xb04040);
    return;
  }
  struct imgblit v;
  v.px = px;
  v.py = py;
  v.iw = iw;
  v.ih = ih;
  v.next_r = 0;
  if (iw <= (unsigned)pw && ih <= (unsigned)ph) {
    v.dw = iw;
    v.dh = ih; /* fits: show 1:1     */
  } else if ((unsigned long)iw * ph > (unsigned long)ih * pw) {
    v.dw = pw;
    v.dh = ih * pw / iw; /* width-bound        */
  } else {
    v.dh = ph;
    v.dw = iw * ph / ih; /* height-bound       */
  }
  if (!v.dw)
    v.dw = 1;
  if (!v.dh)
    v.dh = 1;
  v.ox = ((unsigned)pw - v.dw) / 2;
  v.oy = ((unsigned)ph - v.dh) / 2;

  struct rawrd r2 = {0};
  upng_decode(rawrd_get, &r2, img_row, &v, 0, 0);
}

/* Draw B->pg->raw as a PNG into the content pane (px,py,pw,ph), fit-to-pane
 * with aspect preserved. Decoded once into g_dimg and scale-blitted on later
 * repaints; only a new page / raw buffer forces a re-decode. */
static void draw_image(int px, int py, int pw, int ph, unsigned bg)
{
  (void)bg;
  if (g_dimg.pg != B->pg || g_dimg.src != B->pg->raw || g_dimg.src_len != B->pg->raw_len) {
    free(g_dimg.rgba);
    g_dimg.rgba = 0;
    g_dimg.iw = g_dimg.ih = 0;
    g_dimg.pg = B->pg;
    g_dimg.src = B->pg->raw;
    g_dimg.src_len = B->pg->raw_len;
    struct rawrd r = {0};
    unsigned iw = 0, ih = 0;
    if (upng_dimensions(rawrd_get, &r, &iw, &ih) == 0 && iw && ih && (unsigned long)iw * ih <= DIMG_MAX_PX) {
      unsigned char *buf = malloc((unsigned long)iw * ih * 4);
      if (buf) {
        memset(buf, 0, (unsigned long)iw * ih * 4);
        struct rawrd r2 = {0};
        struct imgc_fill f = {buf, iw, ih};
        if (upng_decode(rawrd_get, &r2, imgc_row, &f, 0, 0) == 0) {
          g_dimg.rgba = buf;
          g_dimg.iw = iw;
          g_dimg.ih = ih;
        } else
          free(buf);
      }
    }
  }
  if (!g_dimg.rgba) {
    draw_image_stream(px, py, pw, ph);
    return;
  } /* too big / unreadable */

  unsigned iw = g_dimg.iw, ih = g_dimg.ih, dw, dh;
  if (iw <= (unsigned)pw && ih <= (unsigned)ph) {
    dw = iw;
    dh = ih; /* fits: show 1:1     */
  } else if ((unsigned long)iw * ph > (unsigned long)ih * pw) {
    dw = pw;
    dh = ih * pw / iw; /* width-bound        */
  } else {
    dh = ph;
    dw = iw * ph / ih; /* height-bound       */
  }
  if (!dw)
    dw = 1;
  if (!dh)
    dh = 1;
  unsigned ox = ((unsigned)pw - dw) / 2, oy = ((unsigned)ph - dh) / 2;
  unsigned *fb = browser_surface();
  for (unsigned ry = 0; ry < dh; ry++) {
    unsigned sy = ry * ih / dh;
    const unsigned char *srow = g_dimg.rgba + (unsigned long)sy * iw * 4;
    unsigned *drow = fb + (unsigned long)(py + oy + ry) * BW + px + ox;
    for (unsigned x = 0; x < dw; x++) {
      const unsigned char *p = srow + (unsigned long)(x * iw / dw) * 4;
      drow[x] = blend_px(p, drow[x]);
    }
  }
}

/* ------------------------------------------------------- inline page images */

/* Byte reader over an inline image's PNG bytes in img_pool. */
struct poolrd {
  int pos, off, len;
};
static int poolrd_get(void *c)
{
  struct poolrd *r = c;
  return r->pos < r->len ? B->img_pool[r->off + r->pos++] : -1;
}

/* Blit one inline image left-aligned at (x0,y0), width dw, height dh, clipped
 * vertically to the content pane [clip_top, clip_bot) so it scrolls cleanly. */
struct inlblit {
  int x0, y0, dw, dh, clip_top, clip_bot;
  unsigned iw, ih, next_r;
};

static int inl_row(void *c, unsigned y, const unsigned char *rgba, unsigned w)
{
  (void)w;
  struct inlblit *v = c;
  unsigned *fb = browser_surface();
  while (v->next_r < (unsigned)v->dh && (v->next_r * v->ih / (unsigned)v->dh) == y) {
    int dy = v->y0 + (int)v->next_r;
    if (dy >= v->clip_top && dy < v->clip_bot) {
      unsigned *drow = fb + (unsigned long)dy * BW + v->x0;
      for (int x = 0; x < v->dw; x++) {
        const unsigned char *p = rgba + ((unsigned)x * v->iw / (unsigned)v->dw) * 4;
        drow[x] = blend_px(p, drow[x]);
      }
    }
    v->next_r++;
  }
  return 0;
}

/* Decoded-PNG cache: an inline PNG would otherwise re-inflate on every scroll
 * frame (SVGs are raw RGBA and blit directly). Each PNG's full RGBA is decoded
 * once, keyed by its session-unique uid, then scaled-blitted thereafter.
 * Bounded by slot count + a per-image pixel cap; cleared when the active page
 * changes or a new page loads. Main-thread-only (render owns it). */
#define IMGC_N 6
#define IMGC_MAX_PX (1024u * 1024u) /* don't cache images bigger than this */
struct imgc_ent {
  int uid;
  unsigned iw, ih;
  unsigned char *rgba;
};
static struct imgc_ent g_imgc[IMGC_N];
static struct page *g_imgc_pg;

static void imgc_clear(void)
{
  for (int i = 0; i < IMGC_N; i++) {
    free(g_imgc[i].rgba);
    g_imgc[i].rgba = 0;
    g_imgc[i].uid = 0;
  }
  free(g_dimg.rgba);
  g_dimg.rgba = 0;
  g_dimg.pg = 0;
  g_dimg.src = 0;
  g_dimg.src_len = 0;
  g_dimg.iw = g_dimg.ih = 0;
}

/* Full decoded RGBA for inline PNG `im` (decoding + caching on first use), or
 * NULL if it can't be decoded / is too large — the caller then streams it once.
 */
static const unsigned char *imgc_get(struct inl_img *im)
{
  if (g_imgc_pg != B->pg) {
    imgc_clear();
    g_imgc_pg = B->pg;
  }
  if (!im->uid || !im->iw || !im->ih)
    return 0;
  for (int i = 0; i < IMGC_N; i++)
    if (g_imgc[i].uid == im->uid && g_imgc[i].rgba)
      return g_imgc[i].rgba;
  if ((unsigned long)im->iw * im->ih > IMGC_MAX_PX)
    return 0;
  unsigned char *buf = malloc((unsigned long)im->iw * im->ih * 4);
  if (!buf)
    return 0;
  memset(buf, 0, (unsigned long)im->iw * im->ih * 4);
  struct poolrd r = {0, im->coff, im->clen};
  struct imgc_fill f = {buf, im->iw, im->ih};
  if (upng_decode(poolrd_get, &r, imgc_row, &f, 0, 0) != 0) {
    free(buf);
    return 0;
  }
  int slot = -1;
  for (int i = 0; i < IMGC_N; i++)
    if (!g_imgc[i].rgba) {
      slot = i;
      break;
    }
  static int rr = 0;
  if (slot < 0) {
    slot = rr;
    rr = (rr + 1) % IMGC_N;
    free(g_imgc[slot].rgba);
  }
  g_imgc[slot].uid = im->uid;
  g_imgc[slot].iw = im->iw;
  g_imgc[slot].ih = im->ih;
  g_imgc[slot].rgba = buf;
  return buf;
}

static void draw_inline_image(struct inl_img *im, int py, int phh, int paw)
{
  /* Draw with the exact geometry the row was reserved with (line_x/line_w),
   * not the full pane: an image inside a narrower/indented box was reserved at
   * its box width but, drawn at pane width, grew taller than its row and lapped
   * the next line. The caller's `paw` is only a fallback for an unrecorded row.
   */
  int row = im->start_row;
  int recorded = (row >= 0 && row < B->nlines);
  int x0 = recorded ? B->line_x[row] : 4;
  int rw = recorded ? B->line_w[row] : (paw < 1 ? 1 : paw);
  int dw = 0, dh = 0;
  img_disp_size(im, rw, &dw, &dh);
  if (dw < 1)
    dw = 1;
  if (dh < 1)
    dh = 1;
  int y0 = py + B->line_y[im->start_row] - B->pg->scroll, ct = py, cb = py + phh;

  /* One scaled blit from a full RGBA source: SVGs live raw in img_pool; PNGs
   * come from the decode cache (decoded once, not per frame). */
  const unsigned char *src = im->is_argb ? (B->img_pool + im->coff) : imgc_get(im);
  if (src) {
    unsigned *fb = browser_surface();
    for (int ry = 0; ry < dh; ry++) {
      int dy = y0 + ry;
      if (dy < ct || dy >= cb)
        continue;
      unsigned sy = (unsigned)ry * im->ih / (unsigned)dh;
      const unsigned char *srow = src + (unsigned long)sy * im->iw * 4;
      unsigned *drow = fb + (unsigned long)dy * BW + x0;
      for (int x = 0; x < dw; x++) {
        const unsigned char *p = srow + ((unsigned)x * im->iw / (unsigned)dw) * 4;
        drow[x] = blend_px(p, drow[x]);
      }
    }
    return;
  }
  /* Fallback (image too large to cache / decode failed): stream it once. */
  struct inlblit v = {x0, y0, dw, dh, ct, cb, im->iw, im->ih, 0};
  struct poolrd r = {0, im->coff, im->clen};
  upng_decode(poolrd_get, &r, inl_row, &v, 0, 0);
}

/* A placeholder box for an image that couldn't be fetched/decoded. */
static void draw_alt_box(struct inl_img *im, int py, int phh)
{
  int y0 = py + B->line_y[im->start_row] - B->pg->scroll;
  if (y0 + LINE_H <= py || y0 >= py + phh)
    return; /* keep it in-pane    */
  const char *t = im->alt[0] ? im->alt : "[image]";
  int w = gfx_ui_width(t) + 8;
  gfx_fill_rect(4, y0, w, LINE_H, 0xe6e9ef);
  gfx_rect(4, y0, w, LINE_H, 0xb4bac4);
  gfx_ui_text(8, y0 + 1, t, 0x7a828c);
}

/* --------------------------------------------------------------- drawing */

/* Draw one wrapped display line in the body font (the page @font-face when set,
 * else the baked gfx_ui font), clipped vertically to [ct,cb). The pane bg is
 * already filled, so the text blends over existing pixels. Bold is a 1px-offset
 * overstrike; underline is a rule under the run. */
/* Draw one wrapped text run [off,off+len) at (x,y), clipped vertically to
 * [ct,cb), in the per-block style (heading size/mono/centre/indent, or a
 * horizontal rule). Returns the content origin x after indent/centring, for the
 * caller's selection highlight. Shared by the main line table and the flex-row
 * overlay. */
static int draw_slice_core(int x, int y, int off, int len, int paw, int ct, int cb, int line_h, int maxpx)
{
  set_block(off);

  /* A horizontal rule is a whole-line element: a centred grey bar, no text. */
  if (cur_bhr) {
    int m = cur_bindent * INDENT_PX;
    gfx_fill_rect(x + m, y + LINE_H / 2, paw - m, 1, 0xc4c8cf);
    return x + m;
  }

  x += cur_bindent * INDENT_PX; /* blockquote / list indent  */
  int base = run_ascent(maxpx); /* baseline shared by the line's runs */
  if (cur_bcenter) {            /* text-align:center         */
    int lw = 0, p = off, end = off + len;
    unsigned cp;
    while (p < end) {
      int gp = p;
      p += utf8_decode(&B->pg->text[p], &B->pg->text[end], &cp);
      lw += body_cp_adv(cp, glyph_px(gp));
    }
    int room = (paw - cur_bindent * INDENT_PX) - lw;
    if (room > 0)
      x += room / 2;
  }
  int x0 = x; /* line content origin (for selection) */

  int i = 0;
  while (i < len) {
    unsigned char a = B->pg->attr[off + i];
    unsigned fg = B->pg->fg[off + i];
    unsigned bgc = B->pg->bg[off + i];
    int rpx = glyph_px(off + i);
    char run[260];
    int rl = 0, rw = 0;
    /* Accumulate codepoints sharing one (attr,fg,bg,size) so any change starts
     * a new run; never split a UTF-8 sequence into garbage. */
    while (i < len && B->pg->attr[off + i] == a && B->pg->fg[off + i] == fg && B->pg->bg[off + i] == bgc &&
           glyph_px(off + i) == rpx) {
      int nb = utf8_seqlen((unsigned char)B->pg->text[off + i]);
      if (i + nb > len)
        nb = 1;
      if (rl + nb > (int)sizeof run - 1)
        break;
      unsigned cp;
      utf8_decode(&B->pg->text[off + i], &B->pg->text[off + len], &cp);
      rw += body_cp_adv(cp, rpx);
      for (int b = 0; b < nb; b++)
        run[rl++] = B->pg->text[off + i++];
    }
    run[rl] = '\0';
    if (bgc != BG_NONE)
      gfx_fill_rect(x, y, rw, line_h, bgc);
    int ital = (a & ST_ITALIC) ? 1 : 0;
    body_draw_run(x, y, run, rl, rw, fg, ct, cb, rpx, base, ital);
    if (a & ST_BOLD)
      body_draw_run(x + 1, y, run, rl, rw, fg, ct, cb, rpx, base, ital);
    if (a & ST_UNDER)
      gfx_fill_rect(x, y + base + 1, rw, 1, fg);
    if (a & ST_STRIKE)
      gfx_fill_rect(x, y + base - rpx / 3, rw, 1, fg);
    x += rw;
  }
  return x0;
}

static void draw_styled_line(int x, int y, int idx, int paw, int ct, int cb)
{
  int off = B->line_off[idx], len = B->line_len[idx];
  int x0 = draw_slice_core(x, y, off, len, paw, ct, cb, B->line_h[idx], B->line_px[idx]);
  if (cur_bhr)
    return; /* rule: nothing to select   */

  /* Text selection: tint the selected byte span [s,e) of this display line. */
  int l0, c0, l1, c1;
  if (gfx_sel_range(&g_sel, &l0, &c0, &l1, &c1) && idx >= l0 && idx <= l1) {
    int s = (idx == l0) ? c0 : 0, e = (idx == l1) ? c1 : len;
    if (s < 0)
      s = 0;
    if (e > len)
      e = len;
    if (e > s) {
      int xs = x0, xe;
      unsigned cp;
      for (int p = off; p < off + s;) {
        int gp = p;
        p += utf8_decode(&B->pg->text[p], &B->pg->text[off + len], &cp);
        xs += body_cp_adv(cp, glyph_px(gp));
      }
      xe = xs;
      for (int p = off + s; p < off + e;) {
        int gp = p;
        p += utf8_decode(&B->pg->text[p], &B->pg->text[off + len], &cp);
        xe += body_cp_adv(cp, glyph_px(gp));
      }
      sel_hl_rect(xs, y, xe - xs, B->line_h[idx]);
    }
  }
}

/* Address-bar row layout: three nav buttons (Back, Forward, Reload) at the
 * left, the URL text field filling the middle, and the bookmark (star) button
 * pinned to the right. One source of truth for render() and the click handler,
 * so the drawn rects and hit rects never drift. */
struct rect {
  int x, y, w, h;
};
#define NAV_BTN_W 28

static void addr_layout(struct rect *back, struct rect *fwd, struct rect *rel, struct rect *tf, struct rect *star)
{
  int y0 = MENU_H + TAB_H;
  back->x = 4;
  back->y = y0 + 3;
  back->w = NAV_BTN_W;
  back->h = ADDR_H - 6;
  fwd->x = back->x + NAV_BTN_W + 2;
  fwd->y = back->y;
  fwd->w = NAV_BTN_W;
  fwd->h = back->h;
  rel->x = fwd->x + NAV_BTN_W + 2;
  rel->y = back->y;
  rel->w = NAV_BTN_W;
  rel->h = back->h;
  star->x = BW - NAV_BTN_W - 4;
  star->y = back->y;
  star->w = NAV_BTN_W;
  star->h = back->h;
  int tx = rel->x + NAV_BTN_W + 6;
  tf->x = tx;
  tf->y = y0 + 4;
  tf->w = star->x - tx - 6;
  tf->h = ADDR_H - 8;
  if (tf->w < 32)
    tf->w = 32; /* keep the field usable on a narrow window */
}

/* A load is in flight when the worker is fetching or a navigation is queued for
 * it — the reload/stop button shows Stop, and Page>Stop is enabled, only then.
 */
static int is_loading(void) { return B->loading || B->req_pending; }

/* The reload (circular arrow) and stop (×) glyphs are hand-rasterized:
 * LiberationSans has no dependable U+21BB, and drawing them keeps the button
 * crisp at any size. Both fill a d×d box at (x0,y0). */
static void icon_reload(int x0, int y0, int d, unsigned col)
{
  int cx = x0 + d / 2, cy = y0 + d / 2, r = d / 2;
  for (int py = -r; py <= r; py++)
    for (int px = -r; px <= r; px++) {
      int dd = px * px + py * py;
      if (dd > r * r || dd < (r - 2) * (r - 2))
        continue;
      if (py < 0 && px >= 0 && px <= 2)
        continue; /* open the ring at the top */
      gfx_pixel(cx + px, cy + py, col);
    }
  /* arrowhead at the gap's right end: a corner stroke that reads as clockwise
   */
  for (int k = 0; k <= 3; k++) {
    gfx_pixel(cx + k, cy - r, col);     /* along the top    */
    gfx_pixel(cx + 3, cy - r + k, col); /* turning downward */
  }
}

static void icon_stop(int x0, int y0, int d, unsigned col)
{
  for (int i = 0; i < d; i++) {
    gfx_pixel(x0 + i, y0 + i, col);
    gfx_pixel(x0 + i + 1, y0 + i, col); /* 2px-thick strokes */
    gfx_pixel(x0 + d - 1 - i, y0 + i, col);
    gfx_pixel(x0 + d - 2 - i, y0 + i, col);
  }
}

/* Draw the combined reload/stop button: the reload arrow while idle, a red × to
 * stop while a load is in flight. */
static void reload_btn_draw(const struct rect *r)
{
  int d = 12;
  int x0 = r->x + (r->w - d) / 2, y0 = r->y + (r->h - d) / 2;
  button_draw(r->x, r->y, r->w, r->h, "", BTN_NORMAL);
  if (is_loading())
    icon_stop(x0, y0, d, 0xe06a5a);
  else
    icon_reload(x0, y0, d, 0xe6ebf2);
}

/* A bookmark-ribbon glyph in a d×d box at (x0,y0): a flag with a V-notch tail,
 * either solid-filled (the page is bookmarked) or a hollow outline (it isn't).
 * Hand-rasterized like the reload/stop icons rather than relying on a font
 * glyph. */
static void icon_bookmark(int x0, int y0, int d, unsigned col, int filled)
{
  int half = d / 2, notch = d / 3;
  for (int y = 0; y < d; y++)
    for (int x = 0; x < d; x++) {
      int ax = x - half;
      if (ax < 0)
        ax = -ax;
      int bottom = (d - 1) - notch + (half ? notch * ax / half : 0);
      if (y > bottom)
        continue; /* below the ribbon / in the tail cut */
      int edge = (x == 0 || x == d - 1 || y == 0 || y == bottom);
      if (filled || edge)
        gfx_pixel(x0 + x, y0 + y, col);
    }
}

/* Draw the bookmark (star) button: a solid gold ribbon when the current page is
 * bookmarked, a hollow grey one otherwise. The button dims on a blank tab (no
 * URL to bookmark), matching the Bookmarks menu's disabled toggle. */
static void star_btn_draw(const struct rect *r)
{
  int has_url = B->pg->cur_url[0] != '\0';
  int marked = has_url && bookmark_find(B->pg->cur_url) != 0;
  int d = 13, x0 = r->x + (r->w - d) / 2, y0 = r->y + (r->h - d) / 2;
  button_draw(r->x, r->y, r->w, r->h, "", has_url ? BTN_NORMAL : BTN_DISABLED);
  if (marked)
    icon_bookmark(x0, y0, d, 0xf2c14e, 1); /* filled gold  */
  else
    icon_bookmark(x0, y0, d, 0x9098a0, 0); /* hollow grey  */
}

/* A display title for the current page: its <title>, else the host, else the
 * URL. */
static void current_page_title(char *out, int cap)
{
  if (B->pg->doc_title[0]) {
    strlcpy(out, B->pg->doc_title, cap);
    return;
  }
  struct purl u;
  if (B->pg->cur_url[0] && parse_url(B->pg->cur_url, &u))
    strlcpy(out, u.host, cap);
  else
    strlcpy(out, B->pg->cur_url[0] ? B->pg->cur_url : "Untitled", cap);
}

/* --- bookmarks panel state --------------------------------------------------
 * * The favicon/folder tree is shown in a left-side panel drawn over the
 * content pane (bookmarks_panel_render below). These track its open state,
 * scroll, the selected node (highlight + the target folder for new
 * bookmarks/folders), and an inline text field used to name a new folder. */
#define BM_PANEL_W 250
#define BM_ROW_H 20
static int bm_panel_open;
static int bm_panel_scroll;         /* first visible row index */
static struct bm_node *bm_selected; /* highlighted node, or NULL */
static struct textfield bm_name_tf; /* inline new-folder name entry */
static char bm_name_buf[64];
static int bm_naming;                  /* 1 = the name field is active */
static struct bm_node *bm_name_parent; /* folder the new folder joins */

/* The folder new bookmarks/folders should join: the selected folder when the
 * panel is open and a folder is picked, else the root. */
static struct bm_node *bm_target_folder(void)
{
  if (bm_panel_open && bm_selected && bm_selected->is_folder)
    return bm_selected;
  return bookmarks_root();
}

/* Add or remove the current page's bookmark (the star button and the Bookmarks
 * menu's item 0 both call this). No-op on a blank tab. A new bookmark captures
 * the page's favicon and joins the target folder. */
static void toggle_current_bookmark(void)
{
  const char *url = B->pg->cur_url;
  if (!url[0])
    return;
  if (bookmark_find(url)) {
    bookmark_remove(url);
  } else {
    char title[128];
    current_page_title(title, sizeof title);
    bookmark_add(bm_target_folder(), url, title, B->pg->favicon_ok ? B->pg->favicon : 0);
  }
  B->seq++; /* repaint: the star + panel reflect the change */
}

/* Start naming a new folder inside `parent` (root if NULL): open the panel,
 * focus an empty inline text field. Enter creates it (handle_key), Esc cancels.
 */
static void begin_new_folder(struct bm_node *parent)
{
  bm_panel_open = 1;
  bm_name_parent = parent ? parent : bookmarks_root();
  bm_naming = 1;
  textfield_set(&bm_name_tf, "");
  B->seq++;
}

/* Refresh the Bookmarks bar menu's dynamic labels + item-0 state. Runs on the
 * main thread right before the menu is drawn or clicked. */
static void bookmarks_menu_sync(void)
{
  int marked = B->pg->cur_url[0] && bookmark_find(B->pg->cur_url) != 0;
  strlcpy(bm_toggle_label, marked ? "Remove Bookmark" : "Add Bookmark", sizeof bm_toggle_label);
  strlcpy(bm_show_label, bm_panel_open ? "Hide Bookmarks" : "Show Bookmarks", sizeof bm_show_label);
  bm_items[0] = bm_toggle_label;
  bm_items[1] = bm_show_label;
  bm_items[2] = "New Folder";
  bm_flags[0] = B->pg->cur_url[0] ? 0 : MENU_DISABLED;
  bm_flags[1] = 0;
  bm_flags[2] = 0;
}

/* The absolute URL of the link currently under the cursor, shown in a status
 * box pinned to the content pane's bottom-left (like Chrome/Firefox). Empty =
 * none. Main-thread-only UI state, so it lives here rather than in the shared
 * struct. */
static char hover_url[URL_MAX];

/* Last pointer Y (from WEV_MOVE/clicks), so the wheel scrolls whichever surface
 * it's over: the devtools dock when the pointer is inside it, else the page. */
static int last_my = -1;
static int last_mx = -1;
static int dt_resizing = 0; /* dragging the devtools panel's resize edge */

/* Set when only the address-bar chrome changed (a URL-field keystroke): the
 * main loop then repaints just that strip instead of re-wrapping and
 * re-blitting the whole page. Distinct from B->seq, which forces a full render.
 */
static int addr_dirty;

/* Copy `src` into `dst` (cap bytes) truncated to fit `maxpx` pixels in the UI
 * font, appending a "..." ellipsis when it had to shorten. Used for the
 * hovered- link status box, where a long URL must not overrun the box. */
static void fit_ui(char *dst, int cap, const char *src, int maxpx)
{
  int n = 0;
  if (gfx_ui_width(src) <= maxpx) { /* whole string fits */
    while (src[n] && n < cap - 1) {
      dst[n] = src[n];
      n++;
    }
    dst[n] = '\0';
    return;
  }
  const char *p = src, *end = src + strlen(src);
  int ell = gfx_ui_width("..."), used = 0;
  while (*p) {
    unsigned cp;
    int nb = utf8_decode(p, end, &cp);
    if (used + gfx_ui_cw_cp(cp) + ell > maxpx)
      break;
    for (int i = 0; i < nb && n < cap - 1; i++)
      dst[n++] = p[i];
    used += gfx_ui_cw_cp(cp);
    p += nb;
  }
  dst[n] = '\0';
  sapp(dst, &n, cap, "...");
}

/* ---- CSS background gradients & url() images --------------------------------
 * Painted per-pixel over a box's (already-filled) background colour. Integer
 * fixed-point throughout (no float): angles via a 5°-step sine table in Q10. */

static int isin90(int d) /* sin(d°), d in [0,90], scaled by 1024 */
{
  static const short t[19] = {0,   89,  178, 265, 350, 433, 512,  587,  658, 724,
                              784, 839, 887, 928, 962, 989, 1008, 1019, 1024};
  if (d < 0)
    d = 0;
  if (d > 90)
    d = 90;
  int i = d / 5, f = d % 5, a = t[i], b = t[i < 18 ? i + 1 : 18];
  return a + (b - a) * f / 5;
}
static int isin(int d)
{
  d %= 360;
  if (d < 0)
    d += 360;
  return d <= 90 ? isin90(d) : d <= 180 ? isin90(180 - d) : d <= 270 ? -isin90(d - 180) : -isin90(360 - d);
}
static int icos(int d) { return isin(d + 90); }

static unsigned isqrtu(unsigned v)
{
  unsigned r = 0, b = 1u << 30;
  while (b > v)
    b >>= 2;
  while (b) {
    if (v >= r + b) {
      v -= r + b;
      r = (r >> 1) + b;
    } else
      r >>= 1;
    b >>= 2;
  }
  return r;
}

/* Gradient stops with positions normalised to percent (autos filled in). */
struct gstops {
  int col[CSS_BG_STOPS];
  int pos[CSS_BG_STOPS];
  int n;
};
static void resolve_stops(const struct css_bg_image *bi, struct gstops *g)
{
  int n = bi->nstops;
  if (n > CSS_BG_STOPS)
    n = CSS_BG_STOPS;
  g->n = n;
  for (int i = 0; i < n; i++) {
    g->col[i] = bi->stops[i].color;
    g->pos[i] = bi->stops[i].pos;
  }
  if (n > 0) {
    if (g->pos[0] < 0)
      g->pos[0] = 0;
    if (g->pos[n - 1] < 0)
      g->pos[n - 1] = 100;
  }
  for (int i = 1; i < n - 1; i++)
    if (g->pos[i] < 0) { /* spread autos toward the next set stop */
      int k = i + 1;
      while (k < n && g->pos[k] < 0)
        k++;
      int next = (k < n) ? g->pos[k] : 100, span = k - (i - 1);
      g->pos[i] = g->pos[i - 1] + (next - g->pos[i - 1]) / (span > 0 ? span : 1);
    }
  for (int i = 1; i < n; i++)
    if (g->pos[i] < g->pos[i - 1])
      g->pos[i] = g->pos[i - 1]; /* monotone */
}

static unsigned lerp_argb(unsigned c0, unsigned c1, int t) /* t in 0..1000 */
{
  int a0 = (c0 >> 24) & 255, r0 = (c0 >> 16) & 255, g0 = (c0 >> 8) & 255, b0 = c0 & 255;
  int a1 = (c1 >> 24) & 255, r1 = (c1 >> 16) & 255, g1 = (c1 >> 8) & 255, b1 = c1 & 255;
  unsigned a = a0 + (a1 - a0) * t / 1000, r = r0 + (r1 - r0) * t / 1000, g = g0 + (g1 - g0) * t / 1000,
           b = b0 + (b1 - b0) * t / 1000;
  return (a << 24) | (r << 16) | (g << 8) | b;
}
static unsigned grad_at(const struct gstops *g, int t1000) /* argb at param t (0..1000) */
{
  if (g->n == 0)
    return 0;
  int t100 = t1000 / 10;
  if (t100 <= g->pos[0])
    return (unsigned)g->col[0];
  if (t100 >= g->pos[g->n - 1])
    return (unsigned)g->col[g->n - 1];
  for (int i = 1; i < g->n; i++)
    if (t100 <= g->pos[i]) {
      int lo = g->pos[i - 1], hi = g->pos[i];
      int seg = hi > lo ? (t1000 - lo * 10) * 1000 / ((hi - lo) * 10) : 0;
      if (seg < 0)
        seg = 0;
      if (seg > 1000)
        seg = 1000;
      return lerp_argb((unsigned)g->col[i - 1], (unsigned)g->col[i], seg);
    }
  return (unsigned)g->col[g->n - 1];
}

/* Composite a straight-ARGB source colour over a framebuffer pixel. */
static unsigned over_argb(unsigned src, unsigned dst)
{
  unsigned a = (src >> 24) & 255;
  if (a == 255)
    return src & 0xffffff;
  if (a == 0)
    return dst;
  unsigned ia = 255 - a;
  unsigned r = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * ia) / 255;
  unsigned g = (((src >> 8) & 255) * a + ((dst >> 8) & 255) * ia) / 255;
  unsigned b = ((src & 255) * a + (dst & 255) * ia) / 255;
  return (r << 16) | (g << 8) | b;
}

/* Paint a linear/radial gradient into the box rect, clipped to the visible band
 * [by,by+bh) and to x in [0,clipw). (bx,sy) is the FULL box top-left in screen
 * px; (bw,bh_full) its full size — so the gradient geometry is
 * band-independent. */
static void paint_gradient(const struct css_bg_image *bi, int bx, int sy, int bw, int bh_full, int by, int bh,
                           int clipw)
{
  if (bw <= 0 || bh_full <= 0)
    return;
  struct gstops g;
  resolve_stops(bi, &g);
  if (g.n == 0)
    return;
  unsigned *fb = browser_surface();
  int x0 = bx < 0 ? 0 : bx, x1 = bx + bw;
  if (x1 > clipw)
    x1 = clipw;
  int cx = bw / 2, cy = bh_full / 2;

  if (bi->kind == CSS_BGK_LINEAR) {
    int dx = isin(bi->angle), dy = -icos(bi->angle); /* unit dir, Q10 */
    long Lq = (long)(dx < 0 ? -dx : dx) * bw + (long)(dy < 0 ? -dy : dy) * bh_full;
    if (Lq < 1)
      Lq = 1; /* Q10 px projection */
    for (int yy = by; yy < by + bh; yy++) {
      int ry = yy - sy;
      long rowdot = (long)(ry - cy) * dy;
      unsigned *drow = fb + (long)yy * BW;
      for (int xx = x0; xx < x1; xx++) {
        long dot = (long)(xx - bx - cx) * dx + rowdot;
        int t = 500 + (int)(dot * 1000 / Lq);
        if (t < 0)
          t = 0;
        if (t > 1000)
          t = 1000;
        drow[xx] = over_argb(grad_at(&g, t), drow[xx]);
      }
    }
  } else { /* radial (centred) */
    int hw = bw / 2 ? bw / 2 : 1, hh = bh_full / 2 ? bh_full / 2 : 1;
    unsigned rad = bi->rad_circle ? isqrtu((unsigned)(hw * hw + hh * hh)) : 0;
    for (int yy = by; yy < by + bh; yy++) {
      int ry = yy - sy - cy;
      unsigned *drow = fb + (long)yy * BW;
      for (int xx = x0; xx < x1; xx++) {
        int rx = xx - bx - cx;
        int t;
        if (bi->rad_circle) {
          unsigned d = isqrtu((unsigned)(rx * rx + ry * ry));
          t = rad ? (int)((long)d * 1000 / rad) : 1000;
        } else { /* ellipse: per-axis normalised, 1000 = edge */
          long nx = (long)rx * 1000 / hw, ny = (long)ry * 1000 / hh;
          t = (int)isqrtu((unsigned)(nx * nx + ny * ny));
        }
        if (t < 0)
          t = 0;
        if (t > 1000)
          t = 1000;
        drow[xx] = over_argb(grad_at(&g, t), drow[xx]);
      }
    }
  }
}

/* Paint a decoded url() background image into the box rect, honouring
 * background- size (cover/contain/length) / -position / -repeat, clipped as
 * paint_gradient. */
static void paint_bg_image(const struct bg_img *im, const struct css_bg_image *bi, int bx, int sy, int bw, int bh_full,
                           int by, int bh, int clipw)
{
  if (!im->rgba || !im->iw || !im->ih || bw <= 0 || bh_full <= 0)
    return;
  int iw = (int)im->iw, ih = (int)im->ih;

  int dw, dh; /* displayed image size */
  switch (bi->size_kind) {
  case CSS_BGS_COVER: {
    int sw = bw * 1000 / iw, sh = bh_full * 1000 / ih, s = sw > sh ? sw : sh;
    dw = iw * s / 1000;
    dh = ih * s / 1000;
    break;
  }
  case CSS_BGS_CONTAIN: {
    int sw = bw * 1000 / iw, sh = bh_full * 1000 / ih, s = sw < sh ? sw : sh;
    dw = iw * s / 1000;
    dh = ih * s / 1000;
    break;
  }
  case CSS_BGS_LEN:
    dw = bi->size_w >= 0 ? bi->size_w : -1;
    dh = bi->size_h >= 0 ? bi->size_h : -1;
    if (dw < 0 && dh < 0) {
      dw = iw;
      dh = ih;
    } else if (dw < 0)
      dw = iw * dh / ih; /* auto keeps aspect */
    else if (dh < 0)
      dh = ih * dw / iw;
    break;
  default:
    dw = iw;
    dh = ih;
    break; /* auto = natural size */
  }
  if (dw < 1)
    dw = 1;
  if (dh < 1)
    dh = 1;

  int posx = bi->pos_x >= 0 ? bi->pos_x : 0, posy = bi->pos_y >= 0 ? bi->pos_y : 0;
  int offx = (bw - dw) * posx / 100, offy = (bh_full - dh) * posy / 100;
  int rep = bi->repeat < 0 ? CSS_BGR_REPEAT : bi->repeat;
  int tilex = (rep == CSS_BGR_REPEAT || rep == CSS_BGR_REPEAT_X);
  int tiley = (rep == CSS_BGR_REPEAT || rep == CSS_BGR_REPEAT_Y);

  unsigned *fb = browser_surface();
  int x0 = bx < 0 ? 0 : bx, x1 = bx + bw;
  if (x1 > clipw)
    x1 = clipw;
  for (int yy = by; yy < by + bh; yy++) {
    int ry = yy - sy - offy;
    if (tiley) {
      ry %= dh;
      if (ry < 0)
        ry += dh;
    } else if (ry < 0 || ry >= dh)
      continue;
    int syimg = ry * ih / dh;
    const unsigned char *srow = im->rgba + (long)syimg * iw * 4;
    unsigned *drow = fb + (long)yy * BW;
    for (int xx = x0; xx < x1; xx++) {
      int rx = xx - bx - offx;
      if (tilex) {
        rx %= dw;
        if (rx < 0)
          rx += dw;
      } else if (rx < 0 || rx >= dw)
        continue;
      const unsigned char *p = srow + (long)(rx * iw / dw) * 4;
      drow[xx] = blend_px(p, drow[xx]);
    }
  }
}

/* Paint visible block-box backgrounds and borders from the box tree's resolved
 * geometry. Called with browser_buf_lock held, after the pane is cleared and rewrap()
 * laid out the tree, before the text lines are drawn. Boxes are in pre-order,
 * so a child paints over its parent. Box `y` is document px (row*LINE_H). */
/* Integer sqrt for the rounded-corner geometry. */
static int isqrt_i(int n)
{
  if (n <= 0)
    return 0;
  int x = n, y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + n / x) / 2;
  }
  return x;
}
/* Fill rounded rect [x,y,w,h] radius r with `rgb` at `alpha` (0..255), clipped
 * to the vertical band [ct,cb). Hard-edged corners; r is clamped to half the
 * smaller side. */
static void fill_round_rect(int x, int y, int w, int h, int r, unsigned rgb, int alpha, int ct, int cb)
{
  if (w <= 0 || h <= 0 || alpha <= 0)
    return;
  if (r < 0)
    r = 0;
  if (r * 2 > w)
    r = w / 2;
  if (r * 2 > h)
    r = h / 2;
  unsigned *fb = browser_surface();
  unsigned sr = (rgb >> 16) & 0xff, sg = (rgb >> 8) & 0xff, sb = rgb & 0xff, ia = 255 - alpha;
  for (int yy = 0; yy < h; yy++) {
    int p_y = y + yy;
    if (p_y < ct || p_y >= cb || p_y < 0 || p_y >= BH)
      continue;
    int cut = 0, d = -1;
    if (yy < r)
      d = r - yy;
    else if (yy >= h - r)
      d = yy - (h - 1 - r);
    if (d >= 0 && d <= r)
      cut = r - isqrt_i(r * r - d * d);
    int xa = x + cut, xb = x + w - cut;
    if (xa < 0)
      xa = 0;
    if (xb > BW)
      xb = BW;
    unsigned *row = fb + (unsigned long)p_y * BW;
    if (alpha >= 255) {
      for (int px = xa; px < xb; px++)
        row[px] = rgb;
    } else
      for (int px = xa; px < xb; px++) {
        unsigned dd = row[px];
        unsigned rr = (sr * alpha + ((dd >> 16) & 0xff) * ia) / 255;
        unsigned gg = (sg * alpha + ((dd >> 8) & 0xff) * ia) / 255;
        unsigned bb = (sb * alpha + (dd & 0xff) * ia) / 255;
        row[px] = (rr << 16) | (gg << 8) | bb;
      }
  }
}
/* A box's drop shadow: an offset, spread, optionally blurred rounded rect. Blur
 * is faked with a few expanding, fading passes (outer faint -> inner solid). */
static void draw_box_shadow(const struct lbox *b, int sy, int r, int ct, int cb)
{
  unsigned argb = b->sh_color;
  int alpha = (int)((argb >> 24) & 0xff);
  if (alpha == 0)
    alpha = 64;
  unsigned rgb = argb & 0xffffff;
  int sp = b->sh_spread, bl = b->sh_blur < 0 ? 0 : b->sh_blur;
  int bx = b->x + b->sh_dx, by = sy + b->sh_dy;
  if (bl <= 0) {
    int g = sp, rr = r + g;
    if (rr < 0)
      rr = 0;
    fill_round_rect(bx - g, by - g, b->w + 2 * g, b->h + 2 * g, rr, rgb, alpha, ct, cb);
    return;
  }
  for (int pass = 0; pass < 3; pass++) { /* outer (faint) -> inner (solid) */
    int g = sp + bl * (2 - pass) / 2;
    int a = alpha * (pass + 1) / 3;
    int rr = r + g;
    if (rr < 0)
      rr = 0;
    fill_round_rect(bx - g, by - g, b->w + 2 * g, b->h + 2 * g, rr, rgb, a, ct, cb);
  }
}

/* Fill a rect clipped to the vertical pane band [ct,cb). */
static void fill_clip(int x, int y, int w, int h, unsigned rgb, int ct, int cb)
{
  int y0 = y < ct ? ct : y, y1 = y + h > cb ? cb : y + h;
  if (y1 > y0 && w > 0)
    gfx_fill_rect(x, y0, w, y1 - y0, rgb);
}

/* Paint one box's shadow, background (colour/gradient/image), border and
 * outline. `alpha` (0..255) is the element's opacity applied to the solid
 * background/border (images/gradients/outline stay opaque — a pragmatic
 * subset). */
static void paint_one_box(struct lbox *b, int py, int pw, int phh, int top_doc)
{
  int bgk = b->bgi.kind; /* CSS_BGK_* image layer, <=0 = none */
  int has_img = bgk == CSS_BGK_LINEAR || bgk == CSS_BGK_RADIAL ||
                (bgk == CSS_BGK_URL && b->bgi.img_idx >= 0 && b->bgi.img_idx < B->pg->n_bgimgs &&
                 B->pg->bgimgs[b->bgi.img_idx].rgba);
  int has_bg = b->bg != BG_NONE;
  int has_bd = b->border_color != BG_NONE && (b->border.t || b->border.r || b->border.b || b->border.l);
  int has_ol = b->outline_w > 0 && b->outline_color != BG_NONE;
  if (!has_bg && !has_bd && !has_img && !has_ol && b->sh_color == BG_NONE)
    return;
  int alpha = b->opacity < 0 ? 255 : b->opacity * 255 / 100;
  int sy = py + b->y - top_doc; /* box top, screen y */
  int r = b->radius;
  int cb = py + phh;
  if (b->sh_color != BG_NONE) /* drop shadow (clips itself to the band) */
    draw_box_shadow(b, sy, r, py, cb);
  int by = sy, bh = b->h;
  if (by < py) {
    bh -= py - by;
    by = py;
  }
  if (by + bh > cb)
    bh = cb - by;
  /* Rounded path only for solid backgrounds; gradients/images stay square. */
  if (bh > 0 && r > 0 && !has_img && has_bg) {
    if (has_bd) { /* border ring: outer colour, inner bg */
      fill_round_rect(b->x, sy, b->w, b->h, r, b->border_color, alpha, py, cb);
      int il = b->border.l, it = b->border.t, ir = b->border.r, ib = b->border.b;
      int bw = il > it ? il : it;
      if (ir > bw)
        bw = ir;
      if (ib > bw)
        bw = ib;
      int inr = r - bw;
      if (inr < 0)
        inr = 0;
      fill_round_rect(b->x + il, sy + it, b->w - il - ir, b->h - it - ib, inr, b->bg, alpha, py, cb);
    } else {
      fill_round_rect(b->x, sy, b->w, b->h, r, b->bg, alpha, py, cb);
    }
  } else if (bh > 0) {
    if (has_bg) {
      if (alpha < 255)
        fill_round_rect(b->x, by, b->w, bh, 0, b->bg, alpha, py, cb);
      else
        gfx_fill_rect(b->x, by, b->w, bh, b->bg);
    }
    if (has_img) {
      if (bgk == CSS_BGK_URL)
        paint_bg_image(&B->pg->bgimgs[b->bgi.img_idx], &b->bgi, b->x, sy, b->w, b->h, by, bh, pw);
      else
        paint_gradient(&b->bgi, b->x, sy, b->w, b->h, by, bh, pw);
    }
    if (has_bd) { /* 4 edges, clipped to the visible band */
      unsigned c = b->border_color;
      if (b->border.t && sy >= py && sy < cb)
        gfx_fill_rect(b->x, sy, b->w, b->border.t, c);
      int bot = sy + b->h - b->border.b;
      if (b->border.b && bot >= py && bot < cb)
        gfx_fill_rect(b->x, bot, b->w, b->border.b, c);
      if (b->border.l)
        gfx_fill_rect(b->x, by, b->border.l, bh, c);
      if (b->border.r)
        gfx_fill_rect(b->x + b->w - b->border.r, by, b->border.r, bh, c);
    }
  }
  if (has_ol) { /* outline: a ring just outside the border box */
    int ow = b->outline_w;
    unsigned oc = b->outline_color;
    fill_clip(b->x - ow, sy - ow, b->w + 2 * ow, ow, oc, py, cb);   /* top    */
    fill_clip(b->x - ow, sy + b->h, b->w + 2 * ow, ow, oc, py, cb); /* bottom */
    fill_clip(b->x - ow, sy, ow, b->h, oc, py, cb);                 /* left   */
    fill_clip(b->x + b->w, sy, ow, b->h, oc, py, cb);               /* right  */
  }
}

/* Paint every box. Boxes paint in pre-order (a child over its parent), but
 * split into three z-index bands so positive z-index lands above the normal
 * flow and negative below — within a band, document order is the tiebreak. */
static void paint_boxes(int py, int pw, int phh)
{
  if (!g_box_layout || B->nlines == 0)
    return;
  int n;
  struct lbox *bx = layout_pool(&n);
  int top_doc = B->pg->scroll; /* document px at the pane top */
  for (int band = 0; band < 3; band++)
    for (int i = 0; i < n; i++) {
      int z = bx[i].z_index;
      int ez = (z == CSS_Z_AUTO) ? 0 : z;
      int g = ez < 0 ? 0 : (ez == 0 ? 1 : 2);
      if (g != band)
        continue;
      /* Cheap vertical cull before the per-box geometry work: skip a box
       * whose extent doesn't reach the visible band [top_doc, top_doc+phh).
       * A shadow/outline spills past the border box, so widen the test by
       * that much to keep an off-screen box's spill correct. */
      struct lbox *b = &bx[i];
      int top = b->y, bot = b->y + b->h;
      if (b->outline_w > 0) {
        top -= b->outline_w;
        bot += b->outline_w;
      }
      if (b->sh_color != BG_NONE) {
        int bl = b->sh_blur < 0 ? 0 : b->sh_blur;
        int ext = b->sh_spread + bl;
        int st = b->y + b->sh_dy - ext, sb = b->y + b->h + b->sh_dy + ext;
        if (st < top)
          top = st;
        if (sb > bot)
          bot = sb;
      }
      if (bot <= top_doc || top >= top_doc + phh)
        continue;
      paint_one_box(b, py, pw, phh, top_doc);
    }
}

/* Outline a devtools-selected element's box over the page: a 2px accent border,
 * each edge drawn only when it actually falls inside the content pane (so a box
 * scrolled partly off-screen doesn't paint a fake edge at the clip line), plus
 * a "tag#id.class WxH" chip at the top-left. Screen coords; clipped to
 * [0,clip_right) x [clip_top,clip_bot). */
static void draw_dt_highlight(int x, int y, int w, int h, int clip_top, int clip_bot, int clip_right, const char *label)
{
  unsigned col = 0x4c8df6;
  int x0 = x < 0 ? 0 : x, x1 = x + w > clip_right ? clip_right : x + w;
  int y0 = y < clip_top ? clip_top : y, y1 = y + h > clip_bot ? clip_bot : y + h;
  if (x1 <= x0 || y1 <= y0)
    return;

  if (y >= clip_top && y < clip_bot)
    gfx_fill_rect(x0, y, x1 - x0, 2, col); /* top    */
  if (y + h - 2 >= clip_top && y + h <= clip_bot)
    gfx_fill_rect(x0, y + h - 2, x1 - x0, 2, col); /* bottom */
  if (x >= 0 && x < clip_right)
    gfx_fill_rect(x, y0, 2, y1 - y0, col); /* left   */
  if (x + w - 2 >= 0 && x + w <= clip_right)
    gfx_fill_rect(x + w - 2, y0, 2, y1 - y0, col); /* right  */

  if (label && label[0]) {
    int lw = gfx_ui_width(label) + 8, lh = UIFONT_H + 4;
    int ly = y0 - lh;
    if (ly < clip_top)
      ly = y0; /* below the top edge if no room above */
    int lx = x0;
    if (lx + lw > clip_right)
      lx = clip_right - lw;
    if (lx < 0)
      lx = 0;
    gfx_fill_rect(lx, ly, lw, lh, 0x2563eb);
    gfx_ui_text(lx + 4, ly + 2, label, 0xffffff);
  }
}

/* Human-readable cause for the "tab crashed" page, from the fault signal. */
static const char *crash_reason(int sig)
{
  switch (sig) {
  case SIGSEGV:
  case SIGBUS:
    return "It ran out of memory or accessed invalid memory.";
  case SIGILL:
    return "It executed an illegal instruction.";
  case SIGFPE:
    return "It hit an arithmetic error.";
  case SIGABRT:
    return "It aborted unexpectedly.";
  default:
    return "Something went wrong while displaying this page.";
  }
}

/* Reload button on the crash page, remembered so handle_click can hit-test it.
 */
static struct rect crash_reload_rect;

/* Draw the "Aw, snap"-style crashed-tab page over the content pane
 * (py..py+phh), replacing the dead page. Centred: a sad face, a heading, the
 * cause, and a Reload button. Its rect is stashed in crash_reload_rect for the
 * click handler. */
static void draw_crash_page(int py, int pw, int phh)
{
  gfx_fill_rect(0, py, pw, phh, 0xf2f3f5); /* neutral canvas, not the dead page's */

  const char *reason = crash_reason(tabs[active_tab].crash_sig);
  const char *head = "This tab has crashed";
  int cx = pw / 2;
  int cy = py + phh / 2;

  int face_px = 64;
  int fw = gfx_ui_width_px(":(", face_px);
  gfx_ui_text_px(cx - fw / 2, cy - 96, ":(", 0xb0b6be, face_px);

  int hw = gfx_ui_width(head);
  gfx_ui_text(cx - hw / 2, cy - 12, head, 0x303840);

  int rw = gfx_ui_width(reason);
  gfx_ui_text(cx - rw / 2, cy + 12, reason, 0x707880);

  int bw = 96, bh = 30;
  crash_reload_rect.x = cx - bw / 2;
  crash_reload_rect.y = cy + 44;
  crash_reload_rect.w = bw;
  crash_reload_rect.h = bh;
  button_draw(crash_reload_rect.x, crash_reload_rect.y, bw, bh, "Reload", BTN_NORMAL);
}

/* --- bookmarks panel: a left-side tree of folders + favicon bookmarks --------
 * Drawn over the content pane when open (below the address bar), so it never
 * reflows the page. Folders expand/collapse; a bookmark shows its 16x16 favicon
 * and navigates on click. New folders (with an inline typed name), the
 * selected- node highlight, delete, and wheel scroll are handled here + in the
 * click/key paths. */

/* Panel geometry: the panel rect, the two header buttons, the inline name
 * field, and the y where the tree rows begin. One source of truth for render +
 * click. */
static void bm_layout(struct rect *nf, struct rect *del, struct rect *name, struct rect *panel, int *list_y)
{
  int top = MENU_H + TAB_H + ADDR_H;
  panel->x = 0;
  panel->y = top;
  panel->w = BM_PANEL_W;
  panel->h = BH - top;
  if (panel->h < 0)
    panel->h = 0;
  int by = top + 24; /* below the header title */
  nf->x = 6;
  nf->y = by;
  nf->w = 96;
  nf->h = 20;
  del->x = 108;
  del->y = by;
  del->w = 60;
  del->h = 20;
  int ny = by + 26;
  name->x = 6;
  name->y = ny;
  name->w = BM_PANEL_W - 12;
  name->h = 20;
  *list_y = ny + (bm_naming ? 26 : 0);
}

/* Flatten the visible tree (a folder contributes its children only when
 * expanded) into a row list shared by the renderer and hit-testing. */
struct bm_row {
  struct bm_node *node;
  int depth;
};
static struct bm_row bm_rows[1024];
static int bm_nrows;
static void bm_flatten_into(struct bm_node *folder, int depth)
{
  for (struct bm_node *n = folder->child; n; n = n->next) {
    if (bm_nrows < (int)(sizeof bm_rows / sizeof bm_rows[0])) {
      bm_rows[bm_nrows].node = n;
      bm_rows[bm_nrows].depth = depth;
      bm_nrows++;
    }
    if (n->is_folder && n->expanded)
      bm_flatten_into(n, depth + 1);
  }
}
static void bm_flatten(void)
{
  bm_nrows = 0;
  bm_flatten_into(bookmarks_root(), 0);
}

/* A small disclosure triangle: right-pointing (collapsed) or down-pointing
 * (open), centred on row-centre `cy`. */
static void bm_tri(int x, int cy, int open, unsigned col)
{
  for (int i = 0; i <= 3; i++)
    for (int j = -(3 - i); j <= (3 - i); j++)
      gfx_pixel(open ? x + j : x + i - 1, open ? cy - 1 + i : cy + j, col);
}

/* A tiny folder glyph anchored at (x,y). */
static void bm_folder_glyph(int x, int y, unsigned col)
{
  gfx_fill_rect(x, y + 1, 5, 2, col);  /* the tab  */
  gfx_fill_rect(x, y + 3, 13, 9, col); /* the body */
}

static void bookmarks_panel_render(void)
{
  struct rect nf, del, name, panel;
  int list_y;
  bm_layout(&nf, &del, &name, &panel, &list_y);
  int top = panel.y, bottom = panel.y + panel.h;

  gfx_fill_rect(panel.x, panel.y, panel.w, panel.h, 0x1b2129);
  gfx_fill_rect(0, top, BM_PANEL_W, 24, 0x232c38); /* header */
  gfx_ui_text(8, top + 3, "Bookmarks", 0xe8eaee);
  button_draw(nf.x, nf.y, nf.w, nf.h, "New Folder", BTN_NORMAL);
  button_draw(del.x, del.y, del.w, del.h, "Delete", bm_selected ? BTN_NORMAL : BTN_DISABLED);
  if (bm_naming)
    textfield_draw(&bm_name_tf, name.x, name.y, name.w, name.h, 1);

  bm_flatten();
  int cap = (bottom - list_y) / BM_ROW_H;
  if (cap < 0)
    cap = 0;
  int maxscroll = bm_nrows - cap;
  if (maxscroll < 0)
    maxscroll = 0;
  if (bm_panel_scroll > maxscroll)
    bm_panel_scroll = maxscroll;
  if (bm_panel_scroll < 0)
    bm_panel_scroll = 0;

  int y = list_y;
  for (int i = bm_panel_scroll; i < bm_nrows && y + BM_ROW_H <= bottom; i++, y += BM_ROW_H) {
    struct bm_node *n = bm_rows[i].node;
    int rx = 6 + bm_rows[i].depth * 14;
    if (n == bm_selected)
      gfx_fill_rect(0, y, BM_PANEL_W, BM_ROW_H, 0x2c4a6e);
    char label[128];
    int tx;
    if (n->is_folder) {
      bm_tri(rx + 2, y + BM_ROW_H / 2, n->expanded, 0xb8c0cc);
      bm_folder_glyph(rx + 10, y + 3, 0xd9a441);
      tx = rx + 28;
    } else {
      if (n->icon)
        gfx_blit_argb(rx + 10, y + 2, FAVICON_W, FAVICON_H, n->icon);
      else {
        gfx_fill_rect(rx + 12, y + 4, 12, 12, 0x4a5568); /* globe placeholder */
        gfx_rect(rx + 12, y + 4, 12, 12, 0x6b7688);
      }
      tx = rx + 30;
    }
    fit_ui(label, sizeof label, n->title ? n->title : "", BM_PANEL_W - tx - 6);
    gfx_ui_text(tx, y + 2, label, n->is_folder ? 0xe8eaee : 0xd2d7dd);
  }
  gfx_fill_rect(BM_PANEL_W - 1, top, 1, panel.h, 0x0f1319); /* right border */
}

/* Route a click inside the bookmarks panel. Returns 1 if it was inside the
 * panel (consumed), 0 to let it fall through to the page. */
static int bm_panel_click(int mx, int my)
{
  struct rect nf, del, name, panel;
  int list_y;
  bm_layout(&nf, &del, &name, &panel, &list_y);
  if (mx < panel.x || mx >= panel.x + panel.w || my < panel.y || my >= panel.y + panel.h)
    return 0;
  if (bm_naming)
    return 1; /* naming: keys, not clicks */
  if (button_hit(nf.x, nf.y, nf.w, nf.h, mx, my)) {
    begin_new_folder(bm_target_folder());
    return 1;
  }
  if (bm_selected && button_hit(del.x, del.y, del.w, del.h, mx, my)) {
    bm_delete(bm_selected);
    bm_selected = 0;
    B->seq++;
    return 1;
  }
  if (my >= list_y) {
    bm_flatten();
    int idx = bm_panel_scroll + (my - list_y) / BM_ROW_H;
    if (idx >= 0 && idx < bm_nrows) {
      struct bm_node *n = bm_rows[idx].node;
      int rx = 6 + bm_rows[idx].depth * 14;
      if (n->is_folder && mx >= rx && mx < rx + 10) {
        folder_toggle(n);
        B->seq++;
        return 1;
      }
      bm_selected = n;
      if (!n->is_folder && n->url)
        navigate(n->url);
      else
        B->seq++;
    } else {
      bm_selected = 0;
      B->seq++;
    }
  }
  return 1;
}

static void render(void)
{
  unsigned bg = B->pg->page_bg;
  /* Hold off any buffer-relocating realloc on the worker thread while we read
   * text/attr/line_off/img_pool/imgs/raw below (see browser_buf_lock in browser.h).
   * render() does only in-memory gfx work — no syscalls that block — so the
   * worker is never stalled for long. */
  browser_buf_lock();

  /* tab strip (between the menu bar and the address bar). Each tab passes its
   * decoded favicon, or NULL so the widget draws the default globe. */
  const char *labels[MAX_TABS];
  const unsigned *icons[MAX_TABS];
  for (int i = 0; i < n_tabs; i++) {
    labels[i] = tabs[i].label[0] ? tabs[i].label : "New Tab";
    icons[i] = tabs[i].page.favicon_ok ? tabs[i].page.favicon : 0;
  }
  tabbar_draw(0, MENU_H, BW, labels, icons, n_tabs, active_tab);

  /* address bar (below the tab strip): Back/Forward buttons + the text field.
   * The buttons dim when their direction has no history (button_hit also
   * refuses a disabled button, so a click on a dim arrow does nothing). */
  gfx_fill_rect(0, MENU_H + TAB_H, BW, ADDR_H, 0x222831);
  struct rect rb, rf, rrl, rtf, rst;
  addr_layout(&rb, &rf, &rrl, &rtf, &rst);
  struct tab *at = &tabs[active_tab];
  button_draw(rb.x, rb.y, rb.w, rb.h, "\xe2\x86\x90", /* ← */
              hist_can_back(at) ? BTN_NORMAL : BTN_DISABLED);
  button_draw(rf.x, rf.y, rf.w, rf.h, "\xe2\x86\x92", /* → */
              hist_can_fwd(at) ? BTN_NORMAL : BTN_DISABLED);
  reload_btn_draw(&rrl);
  textfield_draw(&B->addr_tf, rtf.x, rtf.y, rtf.w, rtf.h, 1);
  star_btn_draw(&rst);

  /* content pane — shrinks to the left of the devtools dock when it is open */
  int right = devtools_visible() ? devtools_left() : BW;
  int py = MENU_H + TAB_H + ADDR_H, pw = right - SCROLL_W;
  int phh = BH - MENU_H - TAB_H - ADDR_H;
  if (phh < 0)
    phh = 0;
  gfx_fill_rect(0, py, pw, phh, bg);

  if (tabs[active_tab].crashed) {
    draw_crash_page(py, pw, phh);
    scrollbar_draw(pw, py, SCROLL_W, phh, 0, 0, 0); /* inert scrollbar */
  } else if (B->pg->mode == MODE_IMAGE) {
    if (B->loading)
      gfx_ui_text(8, py + 8, "Loading ...", 0x4060a0);
    else
      draw_image(0, py, pw, phh, bg);
    scrollbar_draw(pw, py, SCROLL_W, phh, 0, 0, 0); /* inert scrollbar */
  } else {
    int paw = pw - 8; /* content width, px */
    /* The worker owns the line-table flow (see worker_fn). render() only paints
     * the table it built — never reflows — so a heavy page can't stall the
     * event loop here. If the worker hasn't caught up to the current width yet,
     * we paint the existing table for this frame; the worker re-flows within a
     * tick. */
    if (!B->pg->text) {
      B->nlines = 0;
      B->layout_dirty = 1;
    }
    paint_boxes(py, pw, phh); /* block backgrounds behind the text */
    if (B->loading && B->nlines == 0) {
      gfx_ui_text(8, py + 8, "Loading ...", 0x4060a0);
    } else if (B->nlines == 0 && !B->pg->cur_url[0]) {
      /* No page loaded — a fresh tab: prompt for a URL. A page that
       * loaded but produced no lines just stays its blank canvas. */
      gfx_ui_text(8, py + 8, "Type a URL above and press Enter.", 0x707880);
      gfx_ui_text(8, py + 8 + UIFONT_H, "e.g. example.com  or  https://example.com", 0x9098a0);
    }
    int sc = B->pg->scroll;
    for (int i = line_at_px(sc); i < B->nlines; i++) {
      int sy = py + B->line_y[i] - sc;
      if (sy >= py + phh)
        break; /* below the pane: done */
      draw_styled_line(B->line_x[i], sy, i, B->line_w[i], py, py + phh);
    }

    /* flex-row item text, drawn over the reserved band at each item's column */
    for (int q = 0; q < B->n_fslices; q++) {
      struct fslice *f = &B->fslices[q];
      if (f->row < 0 || f->row >= B->nlines)
        continue;
      int sy = py + B->line_y[f->row] - sc;
      if (sy + B->line_h[f->row] <= py || sy >= py + phh)
        continue;
      draw_slice_core(f->x, sy, f->off, f->len, f->w, py, py + phh, f->h, f->maxpx);
    }

    /* inline images, over their reserved (blank) rows */
    for (int q = 0; q < B->pg->n_imgs; q++) {
      struct inl_img *im = &B->pg->imgs[q];
      if (im->start_row < 0)
        continue;
      int r1 = im->start_row + (im->rows > 0 ? im->rows : 1);
      if (r1 > B->nlines)
        r1 = B->nlines;
      int itop = B->line_y[im->start_row], ibot = line_bottom(r1 - 1);
      if (ibot <= sc || itop >= sc + phh)
        continue; /* off-screen */
      /* On-screen now: a deferred loading="lazy" image gets flagged so the
       * worker fetches it (no-op once fetched). */
      if (im->lazy && !im->fetched && show_images)
        im->want = 1;
      if (im->ok && show_images)
        draw_inline_image(im, py, phh, paw);
      else
        draw_alt_box(im, py, phh);
    }

    /* <audio>/<video> control widgets, over their reserved rows */
    for (struct media_el *me = media_doc_first(); me; me = media_doc_next(me)) {
      int sr = media_el_start_row(me);
      if (sr < 0)
        continue;
      int r1 = sr + media_el_rows(me);
      if (r1 > B->nlines)
        r1 = B->nlines;
      int itop = B->line_y[sr], ibot = line_bottom(r1 - 1);
      if (ibot <= sc || itop >= sc + phh)
        continue; /* off-screen */
      media_el_draw(me, 4, py + B->line_y[sr] - sc, paw, py, py + phh);
    }

    /* scrollbar (units are document pixels) */
    scrollbar_draw(pw, py, SCROLL_W, phh, sc, doc_px(), phh);
  }

  /* Hovered-link status: a small flat box pinned to the content pane's bottom-
   * left, like Chrome/Firefox — drawn only while a link is under the cursor, on
   * top of the page but below the menu bar and its dropdowns. */
  if (hover_url[0]) {
    int sy0 = MENU_H + TAB_H + ADDR_H;
    int sh = UIFONT_H + 6;
    int by = BH - sh;
    if (by < sy0)
      by = sy0;
    int maxw = (devtools_visible() ? devtools_left() : BW) - 2;
    char shown[URL_MAX];
    fit_ui(shown, sizeof shown, hover_url, maxw - 12);
    int sw = gfx_ui_width(shown) + 12;
    if (sw > maxw)
      sw = maxw;
    gfx_fill_rect(0, by, sw, sh, 0x202830);
    gfx_rect(0, by, sw, sh, 0x10141a);
    gfx_ui_text(6, by + 3, shown, 0xe8eaee);
  }

  /* Devtools element highlight: outline the selected node's box over the page
   * (HTML pages only; an image page has no DOM to select). The box is document
   * px from the worker, so translate by scroll and clip to the content pane. */
  if (devtools_visible() && B->pg->mode != MODE_IMAGE) {
    int hx, hy, hw, hh;
    const char *hlabel = 0;
    if (devtools_highlight(&hx, &hy, &hw, &hh, &hlabel))
      draw_dt_highlight(hx, py + hy - B->pg->scroll, hw, hh, py, py + phh, pw, hlabel);
  }

  /* Developer tools (F12 / Ctrl+Shift+I): a right-side dock beside the page
   * content (drawn after the page, before the menu dropdowns). No-op when
   * closed. */
  devtools_render();

  /* Bookmarks panel over the content pane (below the menu dropdowns). */
  if (bm_panel_open)
    bookmarks_panel_render();

  /* App menu bar over the top, then the dropdowns last so they overlay the
   * page. "Stop" is only meaningful while a fetch is in flight, so disable it
   * when idle — the widget dims it and ignores clicks/accelerators. */
  page_flags[2] = is_loading() ? 0 : MENU_DISABLED;
  edit_flags[0] = gfx_sel_has(&g_sel) ? 0 : MENU_DISABLED; /* Copy: only with a selection */
  bookmarks_menu_sync();
  gfx_fill_rect(0, 0, BW, MENU_H, 0x232a36);
  menubar_draw(&menubar);

  browser_buf_unlock();
}

/* True while any menu dropdown is expanded. Such a dropdown overlaps the
 * address bar, so the chrome-only fast path must defer to a full render() then.
 */
static int menu_any_open(void)
{
  for (int i = 0; i < menubar.n; i++)
    if (menubar.menus[i] && menubar.menus[i]->open)
      return 1;
  return 0;
}

/* Repaint only the address-bar strip (its background, the Back/Forward buttons,
 * and the URL field) and leave the page untouched. The page below and the menu
 * bar above don't intersect this row, so a present's frame diff blits just this
 * strip — no rewrap, no line/image redraw. Touches no shared content buffers,
 * so unlike render() it needs no browser_buf_lock. */
static void render_addr(void)
{
  gfx_fill_rect(0, MENU_H + TAB_H, BW, ADDR_H, 0x222831);
  struct rect rb, rf, rrl, rtf, rst;
  addr_layout(&rb, &rf, &rrl, &rtf, &rst);
  struct tab *at = &tabs[active_tab];
  button_draw(rb.x, rb.y, rb.w, rb.h, "\xe2\x86\x90", hist_can_back(at) ? BTN_NORMAL : BTN_DISABLED);
  button_draw(rf.x, rf.y, rf.w, rf.h, "\xe2\x86\x92", hist_can_fwd(at) ? BTN_NORMAL : BTN_DISABLED);
  reload_btn_draw(&rrl);
  textfield_draw(&B->addr_tf, rtf.x, rtf.y, rtf.w, rtf.h, 1);
  star_btn_draw(&rst);
}

/* --------------------------------------------------------------- input */

static void pane_metrics(int *py, int *phh, int *rows)
{
  *py = MENU_H + TAB_H + ADDR_H;
  *phh = BH - MENU_H - TAB_H - ADDR_H;
  if (*phh < 0)
    *phh = 0;
  *rows = (*phh > 0) ? *phh / LINE_H : 1;
}

/* Apply a target scroll line, clamped to the page, repainting only when it
 * actually moves — so wheeling or arrowing at an end is a no-op instead of a
 * full content redraw. Callers wanting a tighter bottom stop (e.g. Page Down)
 * pass an already-tighter target; this only re-clamps for safety. */
static void scroll_set(int target)
{
  int max = scroll_max();
  if (target > max)
    target = max;
  if (target < 0)
    target = 0;
  if (target != B->pg->scroll) {
    B->pg->scroll = target;
    B->seq++;
  }
}

static void navigate(const char *url)
{
  hover_url[0] = '\0'; /* drop any stale hovered-link status */
  if (gfx_sel_has(&g_sel))
    gfx_sel_clear(&g_sel);
  g_selecting = 0;
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

static void blur_content_control(void)
{
  if (!B->focus_kind)
    return;
  int k = 0;
  sapp(B->evt_type, &k, sizeof B->evt_type, "blur");
  B->evt_pending = 1;
  B->focus_kind = 0;
}

static void submit(void)
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

/* Reload the current page (reload button, Page>Reload, F5, Ctrl+R). No-op on a
 * blank tab, matching Page>Reload. */
static void do_reload(void)
{
  if (B->pg->cur_url[0])
    navigate(B->pg->cur_url);
}

/* Stop the in-flight load: drop the queued request and signal the worker to
 * bail out of the fetch it is running (polled by the body readers in fetch.c).
 */
static void do_stop(void)
{
  B->req_pending = 0;
  B->abort_load = 1;
  B->seq++;
}

/* Navigate to the active tab's previous/next history entry. The hist index
 * moves here; nav_from_history keeps the load's completion from pushing a new
 * entry. */
static void go_back(void)
{
  struct tab *t = &tabs[active_tab];
  if (!hist_can_back(t))
    return;
  t->hist_i--;
  nav_from_history = 1;
  navigate(t->hist[t->hist_i]);
}

static void go_fwd(void)
{
  struct tab *t = &tabs[active_tab];
  if (!hist_can_fwd(t))
    return;
  t->hist_i++;
  nav_from_history = 1;
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
  B->nlines = 0;
  B->pg->n_imgs = 0;
  B->pg->nlinks = 0;
  B->pg->page_bg = 0xffffff;
  B->pg->page_fg = 0x202830; /* default canvas, not the last page's */
  B->img_pool_len = 0;
  B->layout_dirty = 1;
  B->pg->favicon_ok = 0; /* a blank tab shows the globe */
  B->seq++;
}

/* Pull the freshly-loaded page's url + a label into the active tab (called when
 * a fetch finishes — only the active tab ever loads). The favicon already lives
 * in the page, so the tab strip reads it from tabs[i].page directly. */
static void tab_sync_active(void)
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

/* Reveal tab `i`: point B at its page (instant, no copy), bring its images back
 * into the shared scratch, and repaint. The page is static (its live DOM was
 * dropped when it was last left); a tab interrupted mid-load (no rendered text)
 * is re-fetched. */
static void reveal_tab(int i)
{
  B->pg = &tabs[i].page;           /* the swap — buffers already live here */
  devtools_view_load(&tabs[i].dt); /* this tab's devtools view; drops old capture */
  page_restore_images(&tabs[i].page);
  B->events_live = 0; /* revealed page has no live DOM        */
  B->loading = 0;
  B->req_pending = 0;
  B->layout_dirty = 1; /* force a re-wrap at the live width     */
  textfield_set(&B->addr_tf, tabs[i].url[0] ? tabs[i].url : tabs[i].page.cur_url);
  update_title(B->win_id);
  B->seq++;
  /* A crashed tab keeps its sad page (its empty text isn't "interrupted
   * mid-load"), so don't auto-refetch it — the user reloads it explicitly. */
  if (!tabs[i].crashed && tabs[i].page.text_len == 0 && tabs[i].url[0])
    navigate(tabs[i].url);
}

/* Make tab `i` the active one. Switching is a pointer swap: the page we leave
 * stays put in its tab (DOM dropped, images saved); the one we enter is
 * revealed from its own buffers with no network. */
static void select_tab(int i)
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

static void new_tab(void)
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

static void close_tab(int i)
{
  if (i < 0 || i >= n_tabs || n_tabs <= 1)
    return; /* keep at least one */
  int was_active = (i == active_tab);
  if (was_active)
    load_quiesce(); /* only the active tab loads: stop its fetch first */
  /* Fence the worker: the shift below moves every tab's page struct (and
   * B->pg), which the worker reads each iteration — even a background close
   * relocates the active page's slot. Park regardless of which tab is being
   * closed. */
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

/* Run item `item` of leaf menu `m` (a bar menu or one of its submenus — the
 * menubar helper reports which one, so we dispatch by pointer). Reload is also
 * reached via the F5 accelerator. */
static void menu_action(struct menu *m, int item)
{
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  if (m == &pagemenu) {
    switch (item) {
    case 0:
      do_reload();
      break; /* Reload current page     */
    case 1:
      navigate("example.com");
      break; /* Home                    */
    case 2:
      do_stop();
      break; /* Stop the in-flight load */
    }
  } else if (m == &editmenu) {
    if (item == 0)
      copy_selection(); /* Copy                    */
    else if (item == 1)
      select_all(); /* Select All              */
  } else if (m == &viewmenu) {
    if (item == 0) { /* Show Images checkbox    */
      view_flags[0] ^= MENU_CHECKED;
      show_images = (view_flags[0] & MENU_CHECKED) != 0;
      B->seq++;
    } /* item 1 = Scroll submenu */
  } else if (m == &scrollmenu) {
    switch (item) {
    case 0:
      scroll_set(0);
      break; /* Top      */
    case 1:
      scroll_set(scroll_max());
      break; /* Bottom   */
    case 2:
      scroll_set(B->pg->scroll - phh);
      break; /* Page Up  */
    case 3:
      scroll_set(B->pg->scroll + phh);
      break; /* Page Down */
    }
  } else if (m == &bmarkmenu) {
    switch (item) {
    case 0:
      toggle_current_bookmark();
      break; /* Add/Remove Bookmark */
    case 1:
      bm_panel_open = !bm_panel_open;
      B->seq++;
      break; /* Show/Hide panel     */
    case 2:
      begin_new_folder(bookmarks_root());
      break; /* New top-level folder */
    }
  }
}

/* Hover link-scan cache: the span [hov_lo,hov_hi) of the last resolved link (or
 * of the last empty gap between links) on display row hov_row, valid while the
 * page + its content version (B->pg / B->seq) are unchanged. A pointer move
 * that lands in the same span reuses hov_link instead of rescanning all links —
 * the common case over long link lists / large link-free text areas. */
static struct page *hov_pg;
static unsigned hov_seq;
static int hov_row = -1, hov_link = -1, hov_lo, hov_hi;

/* Line-relative x where display line `row`'s glyphs begin: its left edge plus
 * the block indent and, for a centered block, the centering offset — the same
 * origin draw_slice_core paints from. Calls set_block, so callers must hold
 * browser_buf_lock and the line index must be in range. Shared by the hit-test and
 * selection mappers so neither can drift from the painted position. */
static int line_content_x(int row)
{
  int len = B->line_len[row], base = B->line_off[row];
  set_block(base);
  int x = B->line_x[row] + cur_bindent * INDENT_PX;
  if (cur_bcenter) {
    int lw = 0;
    unsigned cp;
    for (int p = base; p < base + len;) {
      int gp = p;
      p += utf8_decode(&B->pg->text[p], &B->pg->text[base + len], &cp);
      lw += body_cp_adv(cp, glyph_px(gp));
    }
    int room = (B->line_w[row] - cur_bindent * INDENT_PX) - lw;
    if (room > 0)
      x += room / 2;
  }
  return x;
}

/* Resolve a content-pane hit at client coords (mx,my): sets *off to the text
 * offset under the cursor (-1 if outside the pane or past the line's glyphs)
 * and copies the absolute href of any <a> covering it into `url` ("" if none).
 * Holds browser_buf_lock across the walk + resolve, since the worker can move the
 * text/line/ link tables with a realloc mid-load. Shared by the click and hover
 * handlers. */
static void pane_hit(int mx, int my, int *off, char *url, int cap, int *newtab)
{
  *off = -1;
  if (cap)
    url[0] = '\0';
  if (newtab)
    *newtab = 0;
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  int right = devtools_visible() ? devtools_left() : BW;
  if (my < py || my >= py + phh || mx < 0 || mx >= right - SCROLL_W)
    return; /* outside the content pane: skip the lock */
  int row = line_at_px(B->pg->scroll + (my - py));
  browser_buf_lock();
  if (row >= 0 && row < B->nlines) {
    /* proportional: walk the line's glyphs from the painted origin (block
     * indent + centering, via line_content_x) to find which char the point
     * falls on. A point left of the glyphs is not on any char. */
    int len = B->line_len[row], base = B->line_off[row], col = -1;
    int x = line_content_x(row);
    for (int k = 0; mx >= x && k < len;) {
      unsigned cp;
      int nb = utf8_decode(&B->pg->text[base + k], &B->pg->text[base + len], &cp);
      int cw = body_cp_adv(cp, glyph_px(base + k));
      if (mx < x + cw) {
        col = k;
        break;
      }
      x += cw;
      k += nb;
    }
    if (col >= 0)
      *off = base + col;
  }
  /* An inline image (incl. inline <svg>) occupies its own blank reserved row,
   * so the glyph walk above finds no column there — map a click on the image to
   * its anchor offset, so an <a> wrapping the image resolves to that link. */
  if (*off < 0) {
    for (int i = 0; i < B->pg->n_imgs; i++) {
      struct inl_img *im = &B->pg->imgs[i];
      if (im->start_row < 0)
        continue;
      int r1 = im->start_row + (im->rows > 0 ? im->rows : 1);
      if (row >= im->start_row && row < r1) {
        *off = im->text_pos;
        break;
      }
    }
  }
  if (*off >= 0) {
    int cache_ok = (hov_pg == B->pg && hov_seq == B->seq && row == hov_row);
    if (cache_ok && *off >= hov_lo && *off < hov_hi) {
      /* Same row + same link/gap span as the last move: reuse it. */
      if (hov_link >= 0 && hov_link < B->pg->nlinks) {
        resolve_url(B->pg->cur_url, B->pg->links[hov_link].href, url, cap);
        if (newtab)
          *newtab = B->pg->links[hov_link].newtab;
      }
    } else {
      /* Scan once, and cache both the covering link and, on a miss, the
       * link-free gap around *off (bounded by the nearest link edges) so
       * an idle move over plain text also short-circuits next time. */
      int found = -1, lo = 0, hi = B->pg->text_len;
      for (int i = 0; i < B->pg->nlinks; i++) {
        int s = B->pg->links[i].start, e = B->pg->links[i].end;
        if (*off >= s && *off < e) {
          found = i;
          lo = s;
          hi = e;
          break;
        }
        if (e <= *off && e > lo)
          lo = e;
        if (s > *off && s < hi)
          hi = s;
      }
      if (found >= 0) {
        resolve_url(B->pg->cur_url, B->pg->links[found].href, url, cap);
        if (newtab)
          *newtab = B->pg->links[found].newtab;
      }
      hov_pg = B->pg;
      hov_seq = B->seq;
      hov_row = row;
      hov_link = found;
      hov_lo = lo;
      hov_hi = hi;
    }
  }
  browser_buf_unlock();
}

/* Map a pane pixel to (display line, byte offset within that line), clamped so
 * a drag past the text edge selects to the line/end. Used for text selection.
 */
static void pane_linecol(int mx, int my, int *line, int *col)
{
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  *line = 0;
  *col = 0;
  /* The worker rebuilds the line table and B->pg->text under browser_buf_lock; take it
   * so a mid-navigation free/rewrap can't hand this drag a stale nlines>0 with
   * a freed text buffer (utf8_decode would then deref a NULL/dangling pointer).
   */
  browser_buf_lock();
  if (B->pg->text && B->nlines > 0) {
    int row = line_at_px(B->pg->scroll + (my - py));
    if (row < 0)
      row = 0;
    if (row >= B->nlines)
      row = B->nlines - 1;
    *line = row;
    int len = B->line_len[row], base = B->line_off[row];
    int x = line_content_x(row); /* match draw_styled_line */
    int c = len;                 /* default: end of line */
    for (int k = 0; k < len;) {
      unsigned cp;
      int nb = utf8_decode(&B->pg->text[base + k], &B->pg->text[base + len], &cp);
      int cw = body_cp_adv(cp, glyph_px(base + k));
      if (mx < x + cw / 2) {
        c = k;
        break;
      } /* nearest char edge */
      x += cw;
      k += nb;
    }
    *col = c;
  }
  browser_buf_unlock();
}

/* Alpha-blend a translucent selection highlight over the frame buffer rect. */
static void sel_hl_rect(int x, int y, int w, int h)
{
  unsigned *fb = browser_surface();
  const unsigned sr = 0x33, sg = 0x66, sb = 0xcc, a = 110, ia = 255 - a; /* ~43% blue */
  for (int yy = y; yy < y + h; yy++) {
    if (yy < 0 || yy >= BH)
      continue;
    unsigned *row = fb + (unsigned long)yy * BW;
    for (int xx = x; xx < x + w; xx++) {
      if (xx < 0 || xx >= BW)
        continue;
      unsigned d = row[xx];
      unsigned r = (sr * a + ((d >> 16) & 0xff) * ia) / 255;
      unsigned g = (sg * a + ((d >> 8) & 0xff) * ia) / 255;
      unsigned b = (sb * a + (d & 0xff) * ia) / 255;
      row[xx] = (r << 16) | (g << 8) | b;
    }
  }
}

static void handle_click(int mx, int my)
{
  if (my < MENU_H || menu_any_open()) { /* menu bar / dropdown */
    bookmarks_menu_sync();
    struct menu *leaf;
    int item;
    if (menubar_click(&menubar, mx, my, &leaf, &item)) {
      if (item >= 0)
        menu_action(leaf, item);
      B->seq++;
      return;
    }
  }
  /* Developer tools dock (bottom of the window): consumes clicks within it.
   * Its rect sits below the address bar, so it never shadows the bars above. */
  if (devtools_handle_click(mx, my))
    return;
  if (my >= MENU_H && my < MENU_H + TAB_H) { /* tab strip */
    blur_content_control();
    int idx = 0;
    switch (tabbar_hit(0, MENU_H, BW, n_tabs, mx, my, &idx)) {
    case TABBAR_SELECT:
      select_tab(idx);
      break;
    case TABBAR_CLOSE:
      close_tab(idx);
      break;
    case TABBAR_NEW:
      new_tab();
      break;
    }
    B->seq++;
    return;
  }
  if (my >= MENU_H + TAB_H && my < MENU_H + TAB_H + ADDR_H) { /* address bar */
    blur_content_control();
    struct rect rb, rf, rrl, rtf, rst;
    addr_layout(&rb, &rf, &rrl, &rtf, &rst);
    struct tab *at = &tabs[active_tab];
    if (hist_can_back(at) && button_hit(rb.x, rb.y, rb.w, rb.h, mx, my)) {
      go_back();
    } else if (hist_can_fwd(at) && button_hit(rf.x, rf.y, rf.w, rf.h, mx, my)) {
      go_fwd();
    } else if (button_hit(rrl.x, rrl.y, rrl.w, rrl.h, mx, my)) {
      if (is_loading())
        do_stop();
      else
        do_reload();
    } else if (B->pg->cur_url[0] && button_hit(rst.x, rst.y, rst.w, rst.h, mx, my)) {
      toggle_current_bookmark();
    } else {
      textfield_click(&B->addr_tf, mx, my, rtf.x, rtf.y, rtf.w, rtf.h);
    }
    B->seq++;
    return;
  }
  if (bm_panel_open && bm_panel_click(mx, my))
    return;
  int py, phh, rows;
  pane_metrics(&py, &phh, &rows);
  int right = devtools_visible() ? devtools_left() : BW;
  int sbpart = scrollbar_hit(right - SCROLL_W, py, SCROLL_W, phh, mx, my);
  if (sbpart != SCROLLBAR_NONE) { /* scrollbar */
    int target;
    if (sbpart == SCROLLBAR_UP)
      target = B->pg->scroll - 3 * LINE_H;
    else if (sbpart == SCROLLBAR_DOWN)
      target = B->pg->scroll + 3 * LINE_H;
    else
      target = scrollbar_pos(right - SCROLL_W, py, SCROLL_W, phh, mx, my, B->pg->scroll, doc_px(), phh);
    scroll_set(target);
    return;
  }
  if (my >= py && my < py + phh) { /* content pane click */
    /* Crashed tab: the only thing in the pane is the Reload button. */
    if (tabs[active_tab].crashed) {
      if (button_hit(crash_reload_rect.x, crash_reload_rect.y, crash_reload_rect.w, crash_reload_rect.h, mx, my))
        do_reload();
      return;
    }
    /* Inspect-element picker armed: a click selects the element under the
     * cursor in devtools instead of following links / starting a selection. */
    if (devtools_picking()) {
      char res[URL_MAX];
      int off;
      pane_hit(mx, my, &off, res, sizeof res, 0);
      devtools_pick_commit(off);
      return;
    }
    /* A click on an <audio>/<video> controls widget is consumed by it
     * (play/pause, seek, mute) — never treated as a link or selection. */
    {
      int sc = B->pg->scroll;
      int right = devtools_visible() ? devtools_left() : BW;
      int paw = (right - SCROLL_W) - 8;
      if (paw < 8)
        paw = 8;
      for (struct media_el *me = media_doc_first(); me; me = media_doc_next(me)) {
        int sr = media_el_start_row(me);
        if (sr < 0)
          continue;
        int y0 = py + B->line_y[sr] - sc, h = media_el_height(me, paw);
        if (mx >= 4 && mx < 4 + paw && my >= y0 && my < y0 + h) {
          media_el_click(me, mx, my, 4, y0, paw);
          return;
        }
      }
    }
    char res[URL_MAX];
    int off, nt = 0;
    pane_hit(mx, my, &off, res, sizeof res, &nt);
    /* A press clears any previous selection. */
    if (gfx_sel_has(&g_sel)) {
      gfx_sel_clear(&g_sel);
      B->seq++;
    }
    g_selecting = 0;
    /* If the page registered JS listeners, hand the click to the worker to
     * dispatch through the live DOM (it follows the link `res` afterwards
     * unless a handler calls preventDefault). Otherwise take the fast path and
     * navigate directly — pages with no scripts behave exactly as before. */
    if (off >= 0 && B->events_live) {
      /* clientX/clientY are viewport-relative (pane origin), matching
       * getBoundingClientRect; the pane's left edge is window x 0. */
      B->evt_off = off;
      B->evt_x = mx;
      B->evt_y = my - py;
      {
        int k = 0;
        sapp(B->evt_type, &k, sizeof B->evt_type, "click");
      }
      {
        int k = 0;
        sapp(B->evt_text, &k, sizeof B->evt_text, res);
      }
      B->evt_pending = 1;
      B->seq++;
      return;
    }
    if (res[0]) {
      if (nt)
        new_tab(); /* target="_blank": open + switch to a new tab */
      navigate(res);
      return;
    }
    /* Plain text (no link): begin a selection; a following drag extends it. */
    int line, col;
    pane_linecol(mx, my, &line, &col);
    gfx_sel_start(&g_sel, line, col);
    g_selecting = 1;
  }
}

/* All cursor-shape changes funnel through here so the per-element hover shape
 * and the loading spinner share one source of truth; a syscall fires only on a
 * real change (the WM applies w->cursor across the whole client area, so the
 * app must re-pick the shape per pointer position to get element-specific
 * cursors). */
static int cur_cursor = WIN_CURSOR_ARROW;
static void set_cursor(int id, int shape)
{
  if (shape != cur_cursor) {
    win_cursor(id, shape);
    cur_cursor = shape;
  }
}

static int in_rect(int mx, int my, const struct rect *r)
{
  return mx >= r->x && mx < r->x + r->w && my >= r->y && my < r->y + r->h;
}

/* Pick the cursor for the element under (mx,my): I-beam in the address field,
 * hand over links and the clickable chrome (nav buttons, tab strip), else
 * arrow. */
static int cursor_for(int mx, int my, const char *link)
{
  if (mx < 0)
    return WIN_CURSOR_ARROW;
  if (my >= MENU_H && my < MENU_H + TAB_H)
    return WIN_CURSOR_HAND;                                   /* tab strip */
  if (my >= MENU_H + TAB_H && my < MENU_H + TAB_H + ADDR_H) { /* address bar */
    struct rect rb, rf, rrl, rtf, rst;
    addr_layout(&rb, &rf, &rrl, &rtf, &rst);
    if (in_rect(mx, my, &rtf))
      return WIN_CURSOR_TEXT;
    if (in_rect(mx, my, &rb) || in_rect(mx, my, &rf) || in_rect(mx, my, &rrl) || in_rect(mx, my, &rst))
      return WIN_CURSOR_HAND;
    return WIN_CURSOR_ARROW;
  }
  if (bm_panel_open && mx < BM_PANEL_W && my >= MENU_H + TAB_H + ADDR_H)
    return WIN_CURSOR_HAND;
  /* Content pane: I-beam over selectable text, hand over a link, arrow on the
   * scrollbar. */
  if (mx >= (devtools_visible() ? devtools_left() : BW) - SCROLL_W)
    return WIN_CURSOR_ARROW;
  /* Crashed tab: hand over the Reload button, plain arrow elsewhere (nothing to
   * select). */
  if (tabs[active_tab].crashed)
    return in_rect(mx, my, &crash_reload_rect) ? WIN_CURSOR_HAND : WIN_CURSOR_ARROW;
  return link[0] ? WIN_CURSOR_HAND : WIN_CURSOR_TEXT;
}

/* Pointer moved over the content pane with no button held (WEV_MOVE; mx<0 when
 * it left the window): show the hovered link's target in the bottom-left status
 * box, or clear it. Only repaints when the shown URL actually changes. */
static void handle_move(int mx, int my)
{
  char url[URL_MAX];
  int off;
  last_my = mx < 0 ? -1 : my; /* track for wheel routing (devtools vs page) */
  last_mx = mx;
  if (mx < 0) {
    url[0] = '\0';
    off = -1;
  } /* pointer left the window: clear */
  else
    pane_hit(mx, my, &off, url, sizeof url, 0);

  /* Feed the inspect-element picker the page offset under the cursor (only over
   * the page pane, never the devtools panel itself). */
  if (devtools_picking())
    devtools_pick_hover(mx >= 0 && mx < devtools_left() ? off : -1);

  /* Per-element cursor; the loading spinner (event loop) outranks it. */
  if (!B->loading)
    set_cursor(B->win_id, cursor_for(mx, my, url));

  if (streq(url, hover_url))
    return;
  int k = 0;
  sapp(hover_url, &k, sizeof hover_url, url);
  B->seq++;
}

/* Left-button drag in the content pane (WEV_DRAG): extend an in-progress text
 * selection; otherwise treated as a hover move. */
static void handle_drag(int mx, int my)
{
  if (g_selecting) {
    int line, col;
    pane_linecol(mx, my, &line, &col);
    gfx_sel_extend(&g_sel, line, col);
    B->seq++;
    return;
  }
  handle_move(mx, my);
}

/* Copy the current text selection to the clipboard. The display lines are
 * contiguous in B->pg->text, so the selection is one byte span [a,b) — which
 * preserves real newlines (wrap points are spaces, not '\n'). */
static void copy_selection(void)
{
  int l0, c0, l1, c1;
  if (!gfx_sel_range(&g_sel, &l0, &c0, &l1, &c1))
    return;
  browser_buf_lock();
  char *buf = 0;
  int n = 0;
  if (l0 < B->nlines && l1 < B->nlines) {
    int a = B->line_off[l0] + c0, b = B->line_off[l1] + c1;
    if (a < 0)
      a = 0;
    if (b > B->pg->text_len)
      b = B->pg->text_len;
    n = b - a;
    if (n > 0 && (buf = malloc((unsigned)n + 1))) {
      memcpy(buf, B->pg->text + a, (unsigned)n);
      buf[n] = '\0';
    }
  }
  browser_buf_unlock();
  if (buf) {
    clip_put_text(buf, n);
    free(buf);
  }
}

/* Select the whole rendered page (Edit > Select All). */
static void select_all(void)
{
  if (B->nlines <= 0)
    return;
  gfx_sel_start(&g_sel, 0, 0);
  gfx_sel_extend(&g_sel, B->nlines - 1, B->line_len[B->nlines - 1]);
  g_selecting = 0;
  B->seq++;
}

static void handle_key(char c, int mods)
{
  /* Naming a new bookmark folder: the inline field captures every key; Enter
   * commits it, Esc cancels. */
  if (bm_naming) {
    unsigned char uc = (unsigned char)c;
    if (uc == '\n' || uc == '\r') {
      folder_add(bm_name_parent, bm_name_buf);
      bm_naming = 0;
      B->seq++;
    } else if (uc == 27) {
      bm_naming = 0;
      B->seq++;
    } else {
      textfield_key(&bm_name_tf, c);
      B->seq++;
    }
    return;
  }
  /* Ctrl+B toggles the bookmarks panel. */
  if ((mods & KMOD_CTRL) && (c == 'b' || c == 'B')) {
    bm_panel_open = !bm_panel_open;
    B->seq++;
    return;
  }
  /* Developer tools: toggle on F12 or Ctrl+Shift+I; Esc closes an open panel.
   * Ctrl+Shift+I arrives as a bare 'i' char with the CTRL|SHIFT modifier bits.
   */
  if ((unsigned char)c == KEY_F12 || ((mods & KMOD_CTRL) && (mods & KMOD_SHIFT) && (c == 'i' || c == 'I'))) {
    devtools_toggle();
    return;
  }
  /* Ctrl+Shift+C arms the inspect-element picker (like the ↖ toolbar button).
   */
  if ((mods & KMOD_CTRL) && (mods & KMOD_SHIFT) && (c == 'c' || c == 'C')) {
    devtools_pick_toggle();
    return;
  }
  /* The Console REPL input takes keys when it is focused (returns 1); otherwise
   * this is a no-op and the key routes normally (address bar / page). */
  if (devtools_handle_key(c, mods))
    return;
  /* Esc cancels an active pick before it would close the panel. */
  if ((unsigned char)c == 27 && devtools_picking()) {
    devtools_pick_toggle();
    return;
  }
  if ((unsigned char)c == 27 && devtools_visible()) {
    devtools_toggle();
    return;
  }

  /* Alt+Left / Alt+Right are history navigation — checked before the address
   * bar so the modifier turns the arrow into back/forward instead of a caret
   * move. The keyboard reports the chord via mods (KMOD_*); the app decides. */
  if (mods & KMOD_ALT) {
    if ((unsigned char)c == KEY_LEFT) {
      go_back();
      return;
    }
    if ((unsigned char)c == KEY_RIGHT) {
      go_fwd();
      return;
    }
  }

  /* Ctrl+R reloads (the keyboard delivers the unshifted letter + KMOD_CTRL).
   * Checked before the address field so it isn't typed into the URL. */
  if ((mods & KMOD_CTRL) && (c == 'r' || c == 'R')) {
    do_reload();
    return;
  }
  if ((mods & KMOD_CTRL) && (c == 't' || c == 'T')) {
    new_tab();
    return;
  }
  if ((mods & KMOD_CTRL) && (c == 'w' || c == 'W')) {
    close_tab(active_tab);
    return;
  }
  if ((mods & KMOD_CTRL) && c >= '1' && c <= '8') {
    int tab = c - '1';
    if (tab < n_tabs)
      select_tab(tab);
    return;
  }

  /* Ctrl+C (delivered as KEY_INTR) copies the text selection when there is one.
   */
  if ((unsigned char)c == KEY_INTR && gfx_sel_has(&g_sel)) {
    copy_selection();
    return;
  }

  /* Menu accelerator (F5 = Reload): fire the item like a click. */
  struct menu *leaf;
  int sc = menubar_shortcut(&menubar, (unsigned char)c, &leaf);
  if (sc >= 0) {
    menubar_close(&menubar);
    menu_action(leaf, sc);
    return;
  }

  /* A focused content text field captures editing keys: post them to the worker
   * (which owns the DOM) as keyinput/blur events instead of editing the address
   * bar. Esc blurs; other navigation keys fall through to the defaults below.
   */
  if (B->focus_kind != 0) {
    unsigned char uc = (unsigned char)c;
    if (uc == 27) {
      {
        int k = 0;
        sapp(B->evt_type, &k, sizeof B->evt_type, "blur");
      }
      B->evt_pending = 1;
      B->seq++;
      return;
    }
    if (uc == '\n' || uc == '\r' || uc == 8 || uc == 127 || (uc >= 0x20 && uc < 0x7f)) {
      B->evt_x = uc;
      {
        int k = 0;
        sapp(B->evt_type, &k, sizeof B->evt_type, "keyinput");
      }
      B->evt_pending = 1;
      B->seq++;
      return;
    }
  }

  if ((unsigned char)c == 27) {
    menubar_close(&menubar);
    B->seq++;
    return;
  }

  /* The address-bar widget handles editing (printable/Backspace/Delete/caret).
   * That only changes the URL field, so flag a chrome-only repaint rather than
   * bumping B->seq (a full page render) — unless an open dropdown overlaps the
   * strip, where only a full render restores it correctly. */
  if (textfield_key(&B->addr_tf, c)) {
    if (menu_any_open())
      B->seq++;
    else
      addr_dirty = 1;
    return;
  }

  switch ((unsigned char)c) {
  case '\n':
  case '\r':
    submit();
    return;
  case KEY_UP:
    scroll_set(B->pg->scroll - 3 * LINE_H);
    return;
  case KEY_DOWN:
    scroll_set(B->pg->scroll + 3 * LINE_H);
    return;
  }
}

/* The window's owner sets the title once a fetch finishes (worker can't). */
static void update_title(int id)
{
  if (B->pg->doc_title[0]) {
    win_settitle(id, B->pg->doc_title);
    return;
  }
  struct purl u;
  if (B->pg->cur_url[0] && parse_url(B->pg->cur_url, &u)) {
    char t[80];
    int ti = 0;
    sapp(t, &ti, sizeof t, "browser - ");
    sapp(t, &ti, sizeof t, u.host);
    win_settitle(id, t);
    return;
  }
  win_settitle(id, "New Tab"); /* blank tab: match the tab-strip label */
}

int main(int argc, char **argv)
{
  const char *start = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : 0;
  B->pg = &tabs[active_tab].page; /* point at the first tab's page */
  int id = win_create(BW, BH, "browser");
  if (id < 0) {
    puts("browser: no window\n");
    return 1;
  }
  B->win_id = id;
  gfx_init(browser_surface(), BW, BH);
  B->pg->page_bg = 0xffffff;
  B->pg->page_fg = 0x202830; /* until a page sets them */

  textfield_init(&B->addr_tf, B->addr, URL_MAX);
  textfield_init(&bm_name_tf, bm_name_buf, sizeof bm_name_buf);
  if (start)
    textfield_set(&B->addr_tf, start); /* else a blank New Tab (empty bar) */
  B->layout_dirty = 1;

  jar_load();       /* restore cookies + HSTS from the persistent SQLite db */
  perm_load();      /* restore "Allow always" permission grants */
  cache_load();     /* restore the persistent HTTP response cache from disk */
  bookmarks_load(); /* restore the bookmarks tree from disk */

  int wtid = thread_create(worker_fn, &worker_stack[sizeof worker_stack]);

  /* Launched with a URL (e.g. `browser http://host/page`): load it at startup;
   * otherwise open on a blank New Tab rather than prefilling a default site. */
  if (start)
    navigate(start);
  else
    update_title(id); /* blank launch tab: title matches "New Tab" */

  unsigned last = B->seq - 1;
  int prev_loading = 0;
  /* Repaint coalescing while a page loads: async inline-image fetchers bump
   * B->seq as each image lands (resource.c), and a full render() re-lays-out
   * the whole document. At ~66 fps that re-flows the page once per image —
   * wasteful and janky. So while B->loading, throttle full renders to a few per
   * second; the moment loading clears, B->seq != last with loading==0 forces
   * the final render. */
  unsigned frame = 0, last_paint_frame = 0;
  const unsigned LOAD_REDRAW_FRAMES = 6; /* ~6*15ms ≈ 90ms → ~10 fps while loading */
  for (;;) {
    struct win_event ev;
    while (win_poll(id, &ev)) {
      if (ev.type == WEV_CLOSE) {
        worker_quit = 1;
        B->dlg_done = 1; /* release the worker if it's blocked in a dialog */
        if (wtid >= 0)
          thread_join(wtid); /* let the worker exit first */
        win_close(id);
        return 0;
      } else if (ev.type == WEV_KEY) {
        handle_key((char)ev.a, ev.b);
      } else if (ev.type == WEV_MOUSE) {
        last_mx = ev.a;
        last_my = ev.b;
        if (devtools_on_splitter(ev.a, ev.b)) {
          dt_resizing = 1;
        } /* start a panel resize */
        else {
          dt_resizing = 0;
          handle_click(ev.a, ev.b);
        }
      } else if (ev.type == WEV_DRAG) {
        if (dt_resizing)
          devtools_set_width(BW - ev.a); /* docked right: width = BW - x */
        else
          handle_drag(ev.a, ev.b);
      } else if (ev.type == WEV_MOVE) {
        handle_move(ev.a, ev.b);
      } else if (ev.type == WEV_SCROLL) {
        /* The wheel scrolls whichever surface the pointer is over: the
         * bookmarks panel, the devtools dock, else the page. */
        if (bm_panel_open && last_mx >= 0 && last_mx < BM_PANEL_W && last_my >= MENU_H + TAB_H + ADDR_H) {
          bm_panel_scroll -= ev.a;
          if (bm_panel_scroll < 0)
            bm_panel_scroll = 0;
          B->seq++;
        } else if (devtools_visible() && last_mx >= devtools_left() && last_my >= devtools_top()) {
          devtools_handle_scroll(ev.a);
        } else {
          scroll_set(B->pg->scroll - ev.a * 3 * LINE_H); /* wheel up scrolls back */
        }
      } else if (ev.type == WEV_RESIZE) {
        int prev_w = BW, prev_h = BH;
        BW = ev.a;
        BH = ev.b;
        /* Grow the back buffer BEFORE committing the new size: a failed
         * realloc leaves it smaller than BW*BH, and drawing at the new
         * dims would then write past its end (heap OOB). Keep the old
         * size when the grow can't happen — the frame stays at the
         * previous resolution rather than corrupting the heap. */
        browser_surface();
        if (!browser_surface_ready()) {
          BW = prev_w;
          BH = prev_h;
        }
        gfx_init(browser_surface(), BW, BH);
        B->recascade = 1; /* @media may change → worker re-cascades  */
        B->seq++;         /* re-wrap + repaint at the new width */
      }
    }
    /* History API requests posted by JS on the worker (see browser/js/bindings.c).
     * pushState/replaceState is applied before a back/forward posted in the
     * same script tick, so the entry it added is in place first. */
    if (B->hist_nav) { /* pushState / replaceState */
      struct tab *t = &tabs[active_tab];
      B->hist_nav = 0;
      if (B->hist_replace && t->hist_n > 0) {
        int n = (int)strlen(B->hist_url) + 1;
        char *dup = malloc((unsigned)n);
        if (dup) {
          memcpy(dup, B->hist_url, (unsigned)n);
          free(t->hist[t->hist_i]);
          t->hist[t->hist_i] = dup;
        }
      } else {
        hist_push(t, B->hist_url);
      }
      textfield_set(&B->addr_tf, B->hist_url);
      B->seq++;
    }
    if (B->hist_go) { /* history.back/forward/go */
      int d = B->hist_go;
      B->hist_go = 0;
      if (d < 0)
        go_back();
      else
        go_fwd();
    }
    if (B->loc_nav) { /* location.assign/replace/reload */
      B->loc_nav = 0;
      navigate(B->loc_url);
    }
    /* window.alert/confirm/prompt: the JS worker filled B->dlg_* and is blocked
     * on dlg_done. Run the modal dialog HERE, on the main thread (the gfx
     * surface is main-thread-only; the dialog is parented to our window). While
     * the nested dialog loop runs the browser is frozen — correct modal-dialog
     * behaviour. */
    if (B->dlg_req && !B->dlg_done) {
      int req = B->dlg_req, ok = 0;
      if (req == 1) { /* alert */
        dialog_msg(id, B->dlg_title, B->dlg_msg, DLG_ICON_INFO | DLG_OK);
        ok = 1;
      } else if (req == 2) { /* confirm */
        ok = dialog_msg(id, B->dlg_title, B->dlg_msg, DLG_ICON_QUESTION | DLG_OKCANCEL) == DLG_R_OK;
      } else if (req == 4) { /* permission: Refuse/Once/Always */
        static const char *const perm_btns[3] = {"Refuse", "Allow once", "Allow always"};
        /* A locked modal (no close/min/max, not draggable); waits for an
         * answer. */
        int r = dialog_choose(id, B->dlg_title, B->dlg_msg, perm_btns, 3, DLG_ICON_QUESTION);
        ok = r; /* -1 = dismissed via Esc (worker: ask again later) */
      } else {  /* prompt */
        ok = dialog_prompt(id, B->dlg_title, B->dlg_msg, B->dlg_text, B->dlg_text, sizeof B->dlg_text);
      }
      gfx_init(browser_surface(), BW, BH); /* dialog borrowed the surface */
      B->dlg_ok = ok;
      B->dlg_done = 1; /* release the worker (publish last) */
      B->seq++;        /* repaint the browser window */
    }
    B->hist_len = tabs[active_tab].hist_n;

    if (B->loading && !prev_loading)   /* fetch started */
      set_cursor(id, WIN_CURSOR_BUSY); /* spinner while loading */
    if (prev_loading && !B->loading) { /* fetch finished */
      set_cursor(id, WIN_CURSOR_ARROW);
      imgc_clear();      /* drop the old page's decoded images */
      tab_sync_active(); /* refresh the tab label */
      update_title(id);
      if (B->pg->cur_url[0])                        /* show the resolved URL */
        textfield_set(&B->addr_tf, B->pg->cur_url); /* (incl. the scheme) */
      /* Record the page we landed on, unless this load was itself a
       * Back/Next move (then we only walked the existing history). */
      if (!nav_from_history)
        hist_push(&tabs[active_tab], B->pg->cur_url);
      nav_from_history = 0;
      if (B->pg->favicon_ok && B->pg->cur_url[0]) /* update a saved bookmark's icon */
        bookmark_set_icon(B->pg->cur_url, B->pg->favicon);
    }
    prev_loading = B->loading;

    /* While audio plays, nudge a repaint a few times a second so the seek bar
     * and time advance (the worker's media_pump only bumps B->seq on events).
     */
    if (media_active() && (frame - last_paint_frame) >= LOAD_REDRAW_FRAMES)
      B->seq++;

    /* Publish the content-pane pixel width for the worker's line-table flow
     * (must match render()'s paw below: pane width minus the scrollbar and the
     * 8 px left inset). The worker re-wraps when this changes (resize). */
    {
      int right = devtools_visible() ? devtools_left() : BW;
      int wpaw = right - SCROLL_W - 8;
      if (wpaw < 8)
        wpaw = 8;
      B->want_paw = wpaw;
    }

    int due = !B->loading || (frame - last_paint_frame) >= LOAD_REDRAW_FRAMES;
    int dt_paint = devtools_take_paint(); /* devtools-local change (log line) */
    if (B->seq != last && due) {
      render();
      browser_present(id);
      last = B->seq;
      last_paint_frame = frame;
      addr_dirty = 0;
    } else if (addr_dirty || dt_paint) { /* partial repaints: addr bar / devtools dock */
      int addr_paint = addr_dirty;
      if (addr_dirty) {
        render_addr();
        addr_dirty = 0;
      }
      if (dt_paint)
        devtools_render(); /* opaque right-side split: dock only */
      if (addr_paint)
        browser_blit_rect(0, MENU_H + TAB_H, BW, ADDR_H);
      if (dt_paint) {
        int left = devtools_left();
        browser_blit_rect(left, devtools_top(), BW - left, BH - devtools_top());
      }
      win_present(id);
    }
    frame++;
    win_wait(id);
  }
}
