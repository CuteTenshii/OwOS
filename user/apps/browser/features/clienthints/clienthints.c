/* clienthints.c — HTTP Client Hints (Accept-CH opt-in), see clienthints.h.
 *
 * The values are fixed for this browser (one known UA on a known platform), so
 * the only per-origin state is which hints a host asked for. Viewport-Width is
 * the sole dynamic value, read from the live viewport at emit time. */
#include "features/clienthints/clienthints.h"
#include "browser.h"

enum {
  CH_UA = 1u << 0,
  CH_UA_MOBILE = 1u << 1,
  CH_UA_PLATFORM = 1u << 2,
  CH_UA_PLATFORM_VER = 1u << 3,
  CH_UA_ARCH = 1u << 4,
  CH_UA_BITNESS = 1u << 5,
  CH_UA_MODEL = 1u << 6,
  CH_UA_FULL_VER = 1u << 7,
  CH_DPR = 1u << 8,
  CH_VIEWPORT_WIDTH = 1u << 9,
};

/* Each answerable hint: its Accept-CH token, the canonical request-header name,
 * and a static value (NULL for Viewport-Width, computed at emit time). The
 * brand hints keep OwOS's own identity — the Chrome compatibility token in
 * browser_ua() (browser.h) is a UA-string-only concession and deliberately not
 * echoed here. */
struct ch_def {
  unsigned bit;
  const char *token, *name, *value;
};
static const struct ch_def CH_DEFS[] = {
    {CH_UA, "sec-ch-ua", "Sec-CH-UA", "\"OwOS-Browser\";v=\"1\", \"Not.A/Brand\";v=\"99\""},
    {CH_UA_MOBILE, "sec-ch-ua-mobile", "Sec-CH-UA-Mobile", "?0"},
    {CH_UA_PLATFORM, "sec-ch-ua-platform", "Sec-CH-UA-Platform", "\"OwOS\""},
    {CH_UA_PLATFORM_VER, "sec-ch-ua-platform-version", "Sec-CH-UA-Platform-Version", "\"1.0.0\""},
    {CH_UA_ARCH, "sec-ch-ua-arch", "Sec-CH-UA-Arch", "\"x86\""},
    {CH_UA_BITNESS, "sec-ch-ua-bitness", "Sec-CH-UA-Bitness", "\"32\""},
    {CH_UA_MODEL, "sec-ch-ua-model", "Sec-CH-UA-Model", "\"\""},
    {CH_UA_FULL_VER, "sec-ch-ua-full-version-list", "Sec-CH-UA-Full-Version-List",
     "\"OwOS-Browser\";v=\"1.0.0.0\", \"Not.A/Brand\";v=\"99.0.0.0\""},
    {CH_DPR, "dpr", "DPR", "1"},
    {CH_VIEWPORT_WIDTH, "viewport-width", "Viewport-Width", 0},
};
#define CH_NDEFS ((int)(sizeof CH_DEFS / sizeof CH_DEFS[0]))

struct ch_host {
  char *host;
  unsigned mask;
  struct ch_host *next;
};
static struct ch_host *ch_list;

static struct ch_host *ch_find(const char *host)
{
  for (struct ch_host *h = ch_list; h; h = h->next)
    if (streq_ci(h->host, host))
      return h;
  return 0;
}

/* Bit for the Accept-CH token [tok, tok+len), case-insensitively; 0 if unknown.
 */
static unsigned ch_token_bit(const char *tok, int len)
{
  for (int i = 0; i < CH_NDEFS; i++) {
    const char *n = CH_DEFS[i].token;
    int j = 0;
    while (j < len && n[j] && lower(tok[j]) == n[j])
      j++;
    if (j == len && n[j] == '\0')
      return CH_DEFS[i].bit;
  }
  return 0;
}

void ch_accept(const char *host, const char *value)
{
  if (!host || !host[0] || !value)
    return;

  unsigned mask = 0;
  for (const char *p = value; *p;) {
    while (*p == ' ' || *p == '\t' || *p == ',')
      p++;
    const char *tok = p;
    while (*p && *p != ',' && *p != ' ' && *p != '\t')
      p++;
    if (p > tok)
      mask |= ch_token_bit(tok, (int)(p - tok));
  }
  if (!mask)
    return;

  struct ch_host *h = ch_find(host);
  if (h) {
    h->mask |= mask;
    return;
  }

  h = (struct ch_host *)malloc(sizeof *h);
  if (!h)
    return;
  int len = (int)strlen(host);
  h->host = (char *)malloc((unsigned)len + 1);
  if (!h->host) {
    free(h);
    return;
  }
  for (int i = 0; i <= len; i++)
    h->host[i] = host[i];
  h->mask = mask;
  h->next = ch_list;
  ch_list = h;
}

static void put_line(char *out, int *n, int cap, const char *name, const char *value)
{
  for (const char *s = name; *s && *n < cap - 3; s++)
    out[(*n)++] = *s;
  if (*n < cap - 3) {
    out[(*n)++] = ':';
    out[(*n)++] = ' ';
  }
  for (const char *s = value; *s && *n < cap - 3; s++)
    out[(*n)++] = *s;
  out[(*n)++] = '\r';
  out[(*n)++] = '\n';
  out[*n] = '\0';
}

int ch_request_headers(const char *host, char *out, int cap)
{
  if (cap > 0)
    out[0] = '\0';
  if (cap <= 3)
    return 0;
  struct ch_host *h = host ? ch_find(host) : 0;
  if (!h)
    return 0;

  int n = 0;
  for (int i = 0; i < CH_NDEFS; i++) {
    if (!(h->mask & CH_DEFS[i].bit))
      continue;
    if (CH_DEFS[i].value) {
      put_line(out, &n, cap, CH_DEFS[i].name, CH_DEFS[i].value);
    } else { /* Viewport-Width: live viewport */
      int w = 0, vh = 0;
      browser_viewport_px(&w, &vh);
      char num[16];
      int k = 0;
      unsigned v = (unsigned)(w > 0 ? w : 0);
      char t[16];
      int j = 0;
      if (!v)
        t[j++] = '0';
      else
        while (v) {
          t[j++] = (char)('0' + v % 10);
          v /= 10;
        }
      while (j)
        num[k++] = t[--j];
      num[k] = '\0';
      put_line(out, &n, cap, CH_DEFS[i].name, num);
    }
  }
  return n;
}
