/* omnibox.c: address-bar text classification (URL or search terms) and search-URL building. */
#include "features/omnibox/omnibox.h"
#include "owos.h"

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool prefix_ci(const char *s, int n, const char *pre)
{
  int i = 0;
  for (; pre[i]; i++)
    if (i >= n || lower(s[i]) != lower(pre[i]))
      return false;
  return true;
}

/* scheme "://" per RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) */
static bool has_hier_scheme(const char *s, int n)
{
  if (n == 0 || !is_alpha(s[0]))
    return false;
  int i = 1;
  while (i < n && (is_alpha(s[i]) || is_digit(s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.'))
    i++;
  return i + 2 < n && s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/';
}

/* A dotted host name or IPv4 address; a numeric last label is only accepted as a dotted quad. */
static bool is_dotted_host(const char *h, int n)
{
  if (n == 0 || h[0] == '.' || h[n - 1] == '.')
    return false;
  int dots = 0;
  bool all_digits = true;
  for (int i = 0; i < n; i++) {
    char c = h[i];
    if (c == '.') {
      if (h[i - 1] == '.')
        return false;
      dots++;
    } else if (is_digit(c)) {
    } else if (is_alpha(c) || c == '-' || (unsigned char)c >= 0x80) {
      all_digits = false;
    } else {
      return false;
    }
  }
  if (dots == 0)
    return false;
  int last = n;
  while (h[last - 1] != '.')
    last--;
  bool numeric_tld = true;
  for (int i = last; i < n; i++)
    if (!is_digit(h[i]))
      numeric_tld = false;
  return !numeric_tld || (all_digits && dots == 3);
}

bool omnibox_is_url(const char *s, int n)
{
  if (n <= 0)
    return false;
  /* An explicit scheme wins even over spaces, which the fetcher tolerates in paths and data: bodies */
  if (prefix_ci(s, n, "file:") || prefix_ci(s, n, "data:") || prefix_ci(s, n, "blob:") || has_hier_scheme(s, n))
    return true;
  for (int i = 0; i < n; i++)
    if (is_space(s[i]))
      return false;

  int hn = 0;
  while (hn < n && s[hn] != '/' && s[hn] != '?' && s[hn] != '#')
    hn++;
  for (int i = 0; i < hn; i++)
    if (s[i] == '@') /* an email address reads as a search, not userinfo */
      return false;
  if (hn > 0 && s[0] == '[') {
    int i = 1;
    while (i < hn && s[i] != ']')
      i++;
    if (i == hn)
      return false;
    if (i + 1 == hn)
      return true;
    if (s[i + 1] != ':' || i + 2 == hn)
      return false;
    for (int k = i + 2; k < hn; k++)
      if (!is_digit(s[k]))
        return false;
    return true;
  }

  int colon = -1;
  for (int i = 0; i < hn; i++)
    if (s[i] == ':') {
      if (colon >= 0)
        return false;
      colon = i;
    }
  int name_len = colon >= 0 ? colon : hn;
  if (colon >= 0) {
    if (colon + 1 == hn || hn - colon - 1 > 5)
      return false;
    for (int i = colon + 1; i < hn; i++)
      if (!is_digit(s[i]))
        return false;
    if (name_len == 0)
      return false;
  }
  if (name_len == 9 && prefix_ci(s, name_len, "localhost"))
    return true;
  if (is_dotted_host(s, name_len))
    return true;
  /* A bare intranet name with an explicit port ("devbox:8080") */
  if (colon < 0)
    return false;
  for (int i = 0; i < name_len; i++)
    if (!is_alpha(s[i]) && !is_digit(s[i]) && s[i] != '-')
      return false;
  return true;
}

/* application/x-www-form-urlencoded; sets *full and stops before a unit that would not fit */
static int form_encode(const char *s, int n, char *out, int pos, int cap, bool *full)
{
  static const char hex[] = "0123456789ABCDEF";
  for (int i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (is_alpha((char)c) || is_digit((char)c) || c == '*' || c == '-' || c == '.' || c == '_') {
      if (pos + 1 >= cap)
        goto full;
      out[pos++] = (char)c;
    } else if (c == ' ') {
      if (pos + 1 >= cap)
        goto full;
      out[pos++] = '+';
    } else {
      if (pos + 3 >= cap)
        goto full;
      out[pos++] = '%';
      out[pos++] = hex[c >> 4];
      out[pos++] = hex[c & 15];
    }
  }
  return pos;
full:
  *full = true;
  return pos;
}

int omnibox_target(const char *typed, char *out, int cap)
{
  if (cap <= 0)
    return 0;
  out[0] = '\0';
  if (!typed)
    return 0;
  const char *s = typed;
  while (is_space(*s))
    s++;
  int n = (int)strlen(s);
  while (n > 0 && is_space(s[n - 1]))
    n--;
  if (n == 0)
    return 0;

  int pos = 0;
  if (omnibox_is_url(s, n)) {
    for (; pos < n && pos + 1 < cap; pos++)
      out[pos] = s[pos];
  } else {
    static const char prefix[] = OMNIBOX_SEARCH_PREFIX;
    if ((int)sizeof prefix > cap)
      return 0;
    for (; prefix[pos]; pos++)
      out[pos] = prefix[pos];
    bool full = false;
    /* Collapse internal runs of whitespace so "a  b\tc" queries as "a+b+c" */
    for (int i = 0; i < n && !full; i++) {
      if (is_space(s[i])) {
        while (is_space(s[i + 1]))
          i++;
        pos = form_encode(" ", 1, out, pos, cap, &full);
      } else {
        pos = form_encode(s + i, 1, out, pos, cap, &full);
      }
    }
  }
  out[pos] = '\0';
  return pos;
}
