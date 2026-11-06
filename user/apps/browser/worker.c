#include "main_int.h"

/* --- worker thread: drains req_pending, fetches, bumps seq for the UI ----- */
unsigned g_lay_sig; /* layout_sig of the layout currently on screen */

#define RESIZE_SETTLE_MS 50 /* cascade once the window size has been stable this long */
#define RESIZE_MAX_MS 100   /* ...or at least this often during a continuous drag */

/* 256 KiB: http_txn holds hdr[4096] + req[2048] + a cookie buffer, and the
 * redirect follower stacks several URL_MAX (2 KiB) scratch URLs. But the deeper
 * consumer is the recursive DOM render walk (walk_element_inner): it carries a
 * ~552-byte `struct css_style` BY VALUE per frame, so a deeply nested page
 * (hundreds of elements) needs hundreds of KiB or it overflows into B's .bss
 * and corrupts it — surfacing as a browser page-fault AND a worker free() crash
 * at once. Bumped from 64 KiB; ~400 levels of nesting now fit. */
char worker_stack[262144];
volatile bool worker_quit;

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
static volatile bool worker_in_recovery; /* re-entrancy guard: a fault while tearing
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
static bool activate_control(struct dom_node *c, int kind)
{
  switch (kind) {
  case CTRL_CHECKBOX:
    html_toggle_checked(c);
    js_dispatch_dom_event(c, "change", 0, 0);
    html_set_focus(0);
    return true;
  case CTRL_RADIO:
    html_radio_select(c);
    js_dispatch_dom_event(c, "change", 0, 0);
    html_set_focus(0);
    return true;
  case CTRL_SELECT:
    html_select_cycle(c);
    js_dispatch_dom_event(c, "change", 0, 0);
    html_set_focus(0);
    return true;
  case CTRL_TEXT:
    html_set_focus(c);
    return true;
  case CTRL_TEXTAREA:
    if (!dom_get_attr(c, "value"))
      html_seed_textarea(c);
    html_set_focus(c);
    return true;
  case CTRL_SUBMIT:
    html_set_focus(0);
    worker_submit(c);
    return true;
  default:
    html_set_focus(0);
    return true;
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
  bool need_relayout = false;
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
      need_relayout = true;
    } else if ((f = html_focus_edit(ch)) != 0) {
      js_dispatch_dom_event(f, "input", 0, 0);
      need_relayout = true;
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
          need_relayout = true;
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

void worker_fn(void)
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
    g_fr->lt.n = 0; /* the sad-tab page ignores these */
    B->seq++;
    if (!worker_in_recovery) {
      /* First fault for this page: reclaim its DOM/JS heap. Guarded — if the
       * teardown itself refaults on a corrupted heap we land back here with the
       * flag set and skip it, leaking the page rather than looping forever. */
      worker_in_recovery = true;
      html_page_teardown();
      worker_in_recovery = false;
    } else {
      worker_in_recovery = false; /* give up on teardown; stay alive */
    }
  }

  unsigned cascade_ms = 0;
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
    if (B->recascade) { /* window resized: re-run the cascade once the size settles */
      unsigned now = msec();
      if (now - B->resize_ms >= RESIZE_SETTLE_MS || now - cascade_ms >= RESIZE_MAX_MS) {
        B->recascade = 0;
        cascade_ms = now;
        html_relayout(); /* re-evaluates @media at the new size */
        if (B->pg->n_imgs > 0)
          fetch_inline_images(B->pg->cur_url);
        B->seq++;
      }
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
      bool settled = !B->layout_dirty && B->want_paw > 0 && B->wrap_cols == B->want_paw;
      html_relayout();
      if (B->pg->n_imgs > 0)
        fetch_inline_images(B->pg->cur_url);
      if (settled && layout_sig(B->want_paw) == g_lay_sig)
        layout_keep(); /* only paint properties moved: reuse the line table */
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
     * (resize). The result is built off to the side and swapped in under a short
     * browser_buf_lock, so render() never waits on the layout. */
    if (B->want_paw > 0 && (B->layout_dirty || B->wrap_cols != B->want_paw)) {
      rewrap(B->want_paw);
      B->seq++;
    }
    bookmarks_flush(); /* persist a changed bookmarks tree (deferred) */
    msleep(30);
  }
}
