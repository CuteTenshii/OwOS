/* clienthints.c: HTTP Client Hints. Values are static except the viewport. */
#include "features/clienthints/clienthints.h"
#include "mutex.h"
#include "owos.h"
#include "url.h"

void browser_viewport_px(int *w, int *h); /* browser.h; kept out so host tests need no UI headers */

#if defined(__aarch64__)
#define CH_ARCH "arm"
#define CH_BITNESS "64"
#elif defined(__x86_64__)
#define CH_ARCH "x86"
#define CH_BITNESS "64"
#else
#define CH_ARCH "x86"
#define CH_BITNESS "32"
#endif

const struct ch_ua_data ch_ua = {
    .brands = {{"OwOS-Browser", "1"}, {"Not.A/Brand", "99"}},
    .full_brands = {{"OwOS-Browser", "1.0.0.0"}, {"Not.A/Brand", "99.0.0.0"}},
    .platform = "OwOS",
    .platform_version = "1.0.0",
    .architecture = CH_ARCH,
    .bitness = CH_BITNESS,
    .model = "",
    .form_factor = "Desktop",
    .full_version = "1.0.0.0",
};

enum ch_id {
  CH_UA,
  CH_UA_MOBILE,
  CH_UA_PLATFORM,
  CH_UA_PLATFORM_VER,
  CH_UA_ARCH,
  CH_UA_BITNESS,
  CH_UA_MODEL,
  CH_UA_FULL_VER,
  CH_UA_WOW64,
  CH_UA_FORM_FACTORS,
  CH_PREFERS_COLOR_SCHEME,
  CH_PREFERS_REDUCED_MOTION,
  CH_PREFERS_REDUCED_TRANSPARENCY,
  CH_VIEWPORT_WIDTH,
  CH_VIEWPORT_HEIGHT,
  CH_DPR,
  CH_WIDTH,
  CH_DEVICE_MEMORY,
  CH_LEGACY_DPR,
  CH_LEGACY_VIEWPORT_WIDTH,
  CH_LEGACY_DEVICE_MEMORY,
  CH_SAVE_DATA,
  CH_ECT,
  CH_RTT,
  CH_DOWNLINK,
  CH_COUNT
};

/* Accept-CH token, header name, Permissions-Policy feature; low-entropy hints go out without opt-in. */
struct ch_def {
  const char *token, *name, *feature;
  bool low;
};
static const struct ch_def CH_DEFS[CH_COUNT] = {
    [CH_UA] = {"sec-ch-ua", "Sec-CH-UA", "ch-ua", true},
    [CH_UA_MOBILE] = {"sec-ch-ua-mobile", "Sec-CH-UA-Mobile", "ch-ua-mobile", true},
    [CH_UA_PLATFORM] = {"sec-ch-ua-platform", "Sec-CH-UA-Platform", "ch-ua-platform", true},
    [CH_UA_PLATFORM_VER] = {"sec-ch-ua-platform-version", "Sec-CH-UA-Platform-Version", "ch-ua-platform-version"},
    [CH_UA_ARCH] = {"sec-ch-ua-arch", "Sec-CH-UA-Arch", "ch-ua-arch"},
    [CH_UA_BITNESS] = {"sec-ch-ua-bitness", "Sec-CH-UA-Bitness", "ch-ua-bitness"},
    [CH_UA_MODEL] = {"sec-ch-ua-model", "Sec-CH-UA-Model", "ch-ua-model"},
    [CH_UA_FULL_VER] = {"sec-ch-ua-full-version-list", "Sec-CH-UA-Full-Version-List", "ch-ua-full-version-list"},
    [CH_UA_WOW64] = {"sec-ch-ua-wow64", "Sec-CH-UA-WoW64", "ch-ua-wow64"},
    [CH_UA_FORM_FACTORS] = {"sec-ch-ua-form-factors", "Sec-CH-UA-Form-Factors", "ch-ua-form-factors"},
    [CH_PREFERS_COLOR_SCHEME] = {"sec-ch-prefers-color-scheme", "Sec-CH-Prefers-Color-Scheme",
                                 "ch-prefers-color-scheme"},
    [CH_PREFERS_REDUCED_MOTION] = {"sec-ch-prefers-reduced-motion", "Sec-CH-Prefers-Reduced-Motion",
                                   "ch-prefers-reduced-motion"},
    [CH_PREFERS_REDUCED_TRANSPARENCY] = {"sec-ch-prefers-reduced-transparency", "Sec-CH-Prefers-Reduced-Transparency",
                                         "ch-prefers-reduced-transparency"},
    [CH_VIEWPORT_WIDTH] = {"sec-ch-viewport-width", "Sec-CH-Viewport-Width", "ch-viewport-width"},
    [CH_VIEWPORT_HEIGHT] = {"sec-ch-viewport-height", "Sec-CH-Viewport-Height", "ch-viewport-height"},
    [CH_DPR] = {"sec-ch-dpr", "Sec-CH-DPR", "ch-dpr"},
    [CH_WIDTH] = {"sec-ch-width", "Sec-CH-Width", "ch-width"},
    [CH_DEVICE_MEMORY] = {"sec-ch-device-memory", "Sec-CH-Device-Memory", "ch-device-memory"},
    [CH_LEGACY_DPR] = {"dpr", "DPR", "ch-dpr"},
    [CH_LEGACY_VIEWPORT_WIDTH] = {"viewport-width", "Viewport-Width", "ch-viewport-width"},
    [CH_LEGACY_DEVICE_MEMORY] = {"device-memory", "Device-Memory", "ch-device-memory"},
    [CH_SAVE_DATA] = {"save-data", "Save-Data", "ch-save-data"},
    [CH_ECT] = {"ect", "ECT", "ch-ect"},
    [CH_RTT] = {"rtt", "RTT", "ch-rtt"},
    [CH_DOWNLINK] = {"downlink", "Downlink", "ch-downlink"},
};

struct ch_host {
  char *host;
  unsigned mask;
  struct ch_host *next;
};
static struct ch_host *ch_list;
static mutex_t ch_lock = MUTEX_INIT;

static struct ch_host *ch_find(const char *host)
{
  for (struct ch_host *h = ch_list; h; h = h->next)
    if (streq_ci(h->host, host))
      return h;
  return 0;
}

static unsigned ch_host_mask(const char *host)
{
  mutex_lock(&ch_lock);
  struct ch_host *h = ch_find(host);
  unsigned mask = h ? h->mask : 0;
  mutex_unlock(&ch_lock);
  return mask;
}

/* Hint id for an Accept-CH / Critical-CH token (case-insensitive); -1 if unknown. */
static int ch_token_id(const char *tok, int len)
{
  for (int i = 0; i < CH_COUNT; i++) {
    const char *n = CH_DEFS[i].token;
    int j = 0;
    while (j < len && n[j] && lower(tok[j]) == n[j])
      j++;
    if (j == len && n[j] == '\0')
      return i;
  }
  return -1;
}

/* Next list token (parameters skipped); false at the end. */
static bool next_token(const char **p, const char **tok, int *len)
{
  const char *s = *p;
  while (*s == ' ' || *s == '\t' || *s == ',')
    s++;
  if (!*s)
    return false;
  *tok = s;
  while (*s && *s != ',' && *s != ' ' && *s != '\t' && *s != ';')
    s++;
  *len = (int)(s - *tok);
  while (*s && *s != ',')
    s++;
  *p = s;
  return true;
}

void ch_accept(const char *host, const char *value)
{
  if (!host || !host[0] || !value)
    return;

  unsigned mask = 0;
  const char *tok;
  int len;
  for (const char *p = value; next_token(&p, &tok, &len);) {
    int id = ch_token_id(tok, len);
    if (id >= 0)
      mask |= 1u << id;
  }
  if (!mask)
    return;

  mutex_lock(&ch_lock);
  struct ch_host *h = ch_find(host);
  if (h) {
    h->mask |= mask;
    mutex_unlock(&ch_lock);
    return;
  }

  h = (struct ch_host *)malloc(sizeof *h);
  int hlen = (int)strlen(host);
  char *copy = h ? (char *)malloc((unsigned)hlen + 1) : 0;
  if (copy) {
    for (int i = 0; i <= hlen; i++)
      copy[i] = host[i];
    h->host = copy;
    h->mask = mask;
    h->next = ch_list;
    ch_list = h;
  } else {
    free(h);
  }
  mutex_unlock(&ch_lock);
}

static bool is_localhost(const char *host)
{
  int n = (int)strlen(host);
  return streq_ci(host, "localhost") || streq(host, "127.0.0.1") || (n > 10 && streq_ci(host + n - 10, ".localhost"));
}

static bool secure_purl(const struct purl *u) { return u->tls || is_localhost(u->host); }

bool ch_secure_url(const char *url)
{
  struct purl u;
  return url && parse_url(url, &u) && secure_purl(&u);
}

static bool same_origin(const struct purl *a, const struct purl *b)
{
  return a->tls == b->tls && a->port == b->port && streq_ci(a->host, b->host);
}

/* Structured Field list of "brand";v="version" members. */
static void format_brands(char *out, int cap, const struct ch_brand *b)
{
  int n = 0;
  for (int i = 0; i < CH_NBRANDS; i++) {
    n += snprintf(out + n, (unsigned)(cap - n), "%s\"%s\";v=\"%s\"", i ? ", " : "", b[i].brand, b[i].version);
    if (n >= cap)
      return;
  }
}

/* Header value for a hint as a Structured Field item; false when there is no data source. */
static bool ch_value(int id, char *buf, int cap)
{
  int w = 0, h = 0;
  browser_viewport_px(&w, &h);
  switch (id) {
  case CH_UA:
    format_brands(buf, cap, ch_ua.brands);
    return true;
  case CH_UA_FULL_VER:
    format_brands(buf, cap, ch_ua.full_brands);
    return true;
  case CH_UA_MOBILE:
    snprintf(buf, (unsigned)cap, "%s", ch_ua.mobile ? "?1" : "?0");
    return true;
  case CH_UA_WOW64:
    snprintf(buf, (unsigned)cap, "%s", ch_ua.wow64 ? "?1" : "?0");
    return true;
  case CH_UA_PLATFORM:
    snprintf(buf, (unsigned)cap, "\"%s\"", ch_ua.platform);
    return true;
  case CH_UA_PLATFORM_VER:
    snprintf(buf, (unsigned)cap, "\"%s\"", ch_ua.platform_version);
    return true;
  case CH_UA_ARCH:
    snprintf(buf, (unsigned)cap, "\"%s\"", ch_ua.architecture);
    return true;
  case CH_UA_BITNESS:
    snprintf(buf, (unsigned)cap, "\"%s\"", ch_ua.bitness);
    return true;
  case CH_UA_MODEL:
    snprintf(buf, (unsigned)cap, "\"%s\"", ch_ua.model);
    return true;
  case CH_UA_FORM_FACTORS:
    snprintf(buf, (unsigned)cap, "\"%s\"", ch_ua.form_factor);
    return true;
  case CH_PREFERS_COLOR_SCHEME:
    snprintf(buf, (unsigned)cap, "\"light\"");
    return true;
  case CH_PREFERS_REDUCED_MOTION:
  case CH_PREFERS_REDUCED_TRANSPARENCY:
    snprintf(buf, (unsigned)cap, "\"no-preference\"");
    return true;
  case CH_VIEWPORT_WIDTH:
  case CH_LEGACY_VIEWPORT_WIDTH:
    snprintf(buf, (unsigned)cap, "%d", w);
    return true;
  case CH_VIEWPORT_HEIGHT:
    snprintf(buf, (unsigned)cap, "%d", h);
    return true;
  case CH_DPR:
  case CH_LEGACY_DPR:
    snprintf(buf, (unsigned)cap, "1");
    return true;
  case CH_DEVICE_MEMORY:
  case CH_LEGACY_DEVICE_MEMORY:
    snprintf(buf, (unsigned)cap, "2");
    return true;
  case CH_ECT:
    snprintf(buf, (unsigned)cap, "4g");
    return true;
  case CH_RTT:
    snprintf(buf, (unsigned)cap, "50");
    return true;
  case CH_DOWNLINK:
    snprintf(buf, (unsigned)cap, "10");
    return true;
  default: /* Width has no source; Save-Data is only sent when data saving is on */
    return false;
  }
}

/* Append "Name: value\r\n" if the whole line fits. */
static void put_line(char *out, int *n, int cap, const char *name, const char *value)
{
  int need = (int)(strlen(name) + strlen(value)) + 4;
  if (*n + need >= cap)
    return;
  *n += snprintf(out + *n, (unsigned)(cap - *n), "%s: %s\r\n", name, value);
}

/* Last `feature` member of the Permissions-Policy dictionary: sets [*v, *e) to its value. */
static bool policy_member(const char *p, const char *feature, const char **v, const char **e)
{
  bool found = false;
  int flen = (int)strlen(feature);
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',')
      p++;
    const char *key = p;
    while (*p && *p != '=' && *p != ',' && *p != ' ' && *p != '\t')
      p++;
    bool match = (int)(p - key) == flen;
    for (int i = 0; match && i < flen; i++)
      match = lower(key[i]) == feature[i];
    if (*p != '=') {
      while (*p && *p != ',')
        p++;
      continue;
    }
    const char *start = ++p;
    bool quoted = false;
    while (*p && (quoted || *p != ',')) {
      if (*p == '"')
        quoted = !quoted;
      p++;
    }
    if (match) {
      found = true;
      *v = start;
      *e = p;
    }
  }
  return found;
}

/* Whether the allowlist in [v, e) admits `req` from a page at `page`. */
static bool allowlist_admits(const char *v, const char *e, const struct purl *page, const struct purl *req)
{
  while (v < e) {
    if (*v == '*')
      return true;
    if (*v == '"') {
      char item[URL_HOST_MAX + 16];
      int n = 0;
      for (v++; v < e && *v != '"'; v++)
        if (n < (int)sizeof item - 1)
          item[n++] = *v;
      item[n] = '\0';
      struct purl o;
      if (parse_url(item, &o) && same_origin(&o, req))
        return true;
    } else if (v + 4 <= e && starts_ci(v, "self")) {
      if (same_origin(page, req))
        return true;
    }
    while (v < e && *v != ' ' && *v != '(' && *v != ')')
      v++;
    while (v < e && (*v == ' ' || *v == '(' || *v == ')'))
      v++;
  }
  return false;
}

/* Low-entropy hints default to `*`, the rest to `self` (same-origin only). */
static bool delegated(const struct ch_def *d, const char *policy, const struct purl *page, const struct purl *req)
{
  const char *v, *e;
  if (policy && policy_member(policy, d->feature, &v, &e))
    return allowlist_admits(v, e, page, req);
  return d->low;
}

static int build_headers(unsigned optin, const char *policy, const struct purl *page, const struct purl *req, char *out,
                         int cap)
{
  int n = 0;
  bool cross = page && !same_origin(page, req);
  for (int i = 0; i < CH_COUNT; i++) {
    const struct ch_def *d = &CH_DEFS[i];
    char val[160];
    if (!(d->low || (optin & (1u << i))))
      continue;
    if (cross && !delegated(d, policy, page, req))
      continue;
    if (ch_value(i, val, sizeof val))
      put_line(out, &n, cap, d->name, val);
  }
  return n;
}

int ch_request_headers(const char *host, char *out, int cap)
{
  if (cap > 0)
    out[0] = '\0';
  if (cap <= 3 || !host)
    return 0;
  return build_headers(ch_host_mask(host), 0, 0, 0, out, cap);
}

int ch_request_headers_for(const char *page_origin, const char *policy, const char *url, char *out, int cap)
{
  struct purl req, page;
  if (cap > 0)
    out[0] = '\0';
  if (cap <= 3 || !url || !parse_url(url, &req) || !secure_purl(&req))
    return 0;
  bool have_page = page_origin && parse_url(page_origin, &page);
  return build_headers(ch_host_mask(have_page ? page.host : req.host), policy, have_page ? &page : 0, &req, out, cap);
}

static bool has_header(const char *block, const char *name)
{
  int n = (int)strlen(name);
  for (const char *p = block; p && *p;) {
    if (starts_ci(p, name) && p[n] == ':')
      return true;
    while (*p && *p != '\n')
      p++;
    if (*p)
      p++;
  }
  return false;
}

bool ch_critical_retry(const char *critical, const char *sent, const char *now)
{
  const char *tok;
  int len;
  if (!critical || !now)
    return false;
  for (const char *p = critical; next_token(&p, &tok, &len);) {
    int id = ch_token_id(tok, len);
    if (id >= 0 && has_header(now, CH_DEFS[id].name) && !(sent && has_header(sent, CH_DEFS[id].name)))
      return true;
  }
  return false;
}
