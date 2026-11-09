/* hints.c: preconnect and preload on a few background threads, bounded per navigation. */
#include "hints.h"
#include "browser.h"
#include "features/csp/csp.h"
#include "fetchint.h"
#include "mutex.h"
#include "netconn.h"

#define HINT_MAX 16
#define HINT_THREADS 3
#define HINT_STACK (128 * 1024)

struct hint {
  char url[URL_MAX];
  bool connect;
  bool image;
};

static struct hint g_q[HINT_MAX];
static int g_n, g_next, g_active;
static mutex_t g_lock = MUTEX_INIT;

static int g_nthr;
static int g_tids[HINT_MAX];
static char *g_stks[HINT_MAX];
static void *g_tops[HINT_MAX];

static void hint_run(const struct hint *h)
{
  struct purl u;
  if (!parse_url(h->url, &u))
    return;
  if (!u.tls && hsts_has(u.host)) {
    u.tls = 1;
    if (u.port == 80)
      u.port = 443;
  }
  if (h->connect) {
    nc_preconnect(u.host, u.port, u.tls, /*offer_h2=*/1);
    return;
  }
  struct http_req rq = {0};
  rq.url = h->url;
  rq.accept = h->image ? IMG_ACCEPT : SUBRES_ACCEPT;
  struct http_resp rs = {0};
  free(browser_fetch(&rq, &rs));
}

static void hint_thread(void)
{
  for (;;) {
    struct hint h;
    mutex_lock(&g_lock);
    if (g_next >= g_n) {
      g_active--;
      mutex_unlock(&g_lock);
      break;
    }
    h = g_q[g_next++];
    mutex_unlock(&g_lock);
    hint_run(&h);
  }
  exit(0);
}

static bool hint_spawn(void)
{
  char *stk = 0;
  void *top = thread_stack_alloc(HINT_STACK);
  if (!top) {
    stk = malloc(HINT_STACK);
    if (!stk)
      return false;
    top = stk + HINT_STACK;
  }
  int tid = thread_create(hint_thread, top);
  if (tid < 0) {
    if (stk)
      free(stk);
    else
      thread_stack_free(top);
    return false;
  }
  g_stks[g_nthr] = stk;
  g_tops[g_nthr] = top;
  g_tids[g_nthr++] = tid;
  return true;
}

static void hint_add(const char *url, bool connect, bool image)
{
  if (!starts_ci(url, "http://") && !starts_ci(url, "https://"))
    return;
  mutex_lock(&g_lock);
  for (int i = 0; i < g_n; i++)
    if (g_q[i].connect == connect && streq(g_q[i].url, url)) {
      mutex_unlock(&g_lock);
      return;
    }
  if (g_n >= HINT_MAX) {
    mutex_unlock(&g_lock);
    return;
  }
  struct hint *h = &g_q[g_n++];
  int k = 0;
  h->url[0] = '\0';
  sapp(h->url, &k, sizeof h->url, url);
  h->connect = connect;
  h->image = image;
  bool spawn = g_active < HINT_THREADS;
  if (spawn)
    g_active++;
  mutex_unlock(&g_lock);
  if (spawn && !hint_spawn()) {
    mutex_lock(&g_lock);
    g_active--;
    mutex_unlock(&g_lock);
  }
}

/* True if `tok` is a whitespace-delimited token of the rel value in [q,end). */
bool hint_rel_has(const char *q, const char *end, const char *tok)
{
  int tl = strlen(tok);
  for (const char *s = q; s < end;) {
    while (s < end && (*s == ' ' || *s == '\t' || *s == '"'))
      s++;
    const char *t = s;
    while (s < end && *s != ' ' && *s != '\t' && *s != '"')
      s++;
    if (s - t == tl && starts_ci(t, tok))
      return true;
  }
  return false;
}

void hint_preconnect(const char *abs_url) { hint_add(abs_url, true, false); }

void hint_preload(const char *abs_url, const char *as, const char *page_url)
{
  int type;
  if (streq_ci(as, "style"))
    type = CSP_STYLE;
  else if (streq_ci(as, "font"))
    type = CSP_FONT;
  else if (streq_ci(as, "script"))
    type = CSP_SCRIPT;
  else if (streq_ci(as, "image"))
    type = CSP_IMG;
  else
    return;
  char use[URL_MAX], reason[64];
  const char *url = abs_url;
  if (B->pg->csp) {
    if (!csp_check_url(B->pg->csp, type, abs_url, page_url, use, sizeof use, reason, sizeof reason))
      return;
    url = use;
  }
  hint_add(url, false, type == CSP_IMG);
}

void hints_join(void)
{
  mutex_lock(&g_lock);
  g_next = g_n;
  mutex_unlock(&g_lock);
  for (int t = 0; t < g_nthr; t++) {
    thread_join(g_tids[t]);
    if (g_stks[t])
      free(g_stks[t]);
    else
      thread_stack_free(g_tops[t]);
  }
  g_nthr = 0;
  mutex_lock(&g_lock);
  g_n = g_next = g_active = 0;
  mutex_unlock(&g_lock);
}
