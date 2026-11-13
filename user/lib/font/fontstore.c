#include "fontstore.h"
#include "font.h"
#include "owos.h"

#define DIR "/etc/fonts"
#define CONF "/etc/fonts.conf"
#define NAME_MAX_U 64

static const char *const key[FONT_ROLES] = {"sans=", "mono=", "emoji=", "cjk="};
static const char *const builtin[FONT_ROLES] = {"LiberationSans-Regular.ttf", "LiberationMono-Regular.ttf",
                                                "NotoColorEmoji.ttf", "NotoSansCJKsc-Regular.otf"};

/* Web-font containers; present only in programs that link user/lib/font/woff.c */
extern int woff_is_woff(const unsigned char *, int) __attribute__((weak));
extern unsigned char *woff_to_sfnt(const unsigned char *, int, int *) __attribute__((weak));

/* Length of the extension `ext` (with dot) that ends `s` ignoring case, else 0 */
static int ext_len(const char *s, const char *ext)
{
  int n = (int)strlen(s), e = (int)strlen(ext);
  if (n <= e)
    return 0;
  for (int i = 0; i < e; i++)
    if ((s[n - e + i] | 32) != ext[i])
      return 0;
  return e;
}

/* Installed fonts are always plain SFNT files */
static int is_font_name(const char *s)
{
  int n = (int)strlen(s);
  return n < NAME_MAX_U && (ext_len(s, ".ttf") || ext_len(s, ".otf")) && !strchr(s, '/');
}

static const char *after(const char *s, const char *prefix)
{
  for (; *prefix; s++, prefix++)
    if (*s != *prefix)
      return 0;
  return s;
}

static unsigned char *slurp(const char *path, int *len)
{
  struct statinfo st;
  if (stat(path, &st) < 0 || st.is_dir || st.size == 0)
    return 0;
  unsigned char *buf = malloc(st.size);
  if (buf && readfile_at(path, 0, buf, st.size) != (int)st.size) {
    free(buf);
    return 0;
  }
  *len = (int)st.size;
  return buf;
}

int fontstore_install(const char *src, char *name)
{
  const char *base = src;
  for (const char *p = src; *p; p++)
    if (*p == '/')
      base = p + 1;
  int blen = (int)strlen(base), web = ext_len(base, ".woff2") ? ext_len(base, ".woff2") : ext_len(base, ".woff");
  if (blen >= NAME_MAX_U || !(web || ext_len(base, ".ttf") || ext_len(base, ".otf")))
    return -1;
  int len;
  unsigned char *buf = slurp(src, &len);
  if (!buf)
    return -1;
  if (web) {
    int sl = 0;
    unsigned char *sfnt = woff_is_woff && woff_to_sfnt && woff_is_woff(buf, len) ? woff_to_sfnt(buf, len, &sl) : 0;
    free(buf);
    if (!sfnt)
      return -1;
    buf = sfnt;
    len = sl;
  }
  struct font *f = font_load(buf, len);
  int rc = -1;
  if (f) {
    font_free(f);
    char dst[96];
    int otf = len >= 4 && !memcmp(buf, "OTTO", 4);
    if (web) {
      memcpy(name, base, (unsigned)(blen - web));
      strcpy(name + blen - web, otf ? ".otf" : ".ttf");
    } else
      strcpy(name, base);
    snprintf(dst, sizeof dst, DIR "/%s", name);
    mkdir(DIR);
    rc = writefile(dst, buf, len);
  }
  free(buf);
  return rc;
}

int fontstore_list(int i, char *name)
{
  struct dirent_u de;
  for (int idx = 0, seen = 0; readdir(DIR, idx, &de) == 1; idx++) {
    if (de.is_dir || !is_font_name(de.name) || !strcmp(de.name, builtin[FONT_EMOJI]) ||
        !strcmp(de.name, builtin[FONT_CJK]))
      continue;
    if (seen++ == i) {
      strcpy(name, de.name);
      return 0;
    }
  }
  return -1;
}

static int conf_get(int role, char *name)
{
  char buf[FONT_ROLES * (NAME_MAX_U + 8) + 1];
  int n = readfile(CONF, buf, sizeof buf - 1);
  if (n <= 0)
    return 0;
  buf[n] = '\0';
  for (char *p = buf; *p;) {
    char *e = p;
    while (*e && *e != '\n')
      e++;
    int had_nl = *e == '\n';
    *e = '\0';
    const char *v = after(p, key[role]);
    if (v && is_font_name(v)) {
      strcpy(name, v);
      return 1;
    }
    p = e + had_nl;
  }
  return 0;
}

void fontstore_current(int role, char *name)
{
  char path[96];
  struct statinfo st;
  if (conf_get(role, name)) {
    snprintf(path, sizeof path, DIR "/%s", name);
    if (stat(path, &st) == 0 && !st.is_dir)
      return;
  }
  strcpy(name, builtin[role]);
}

void fontstore_path(int role, char *path, int n)
{
  char name[NAME_MAX_U];
  fontstore_current(role, name);
  snprintf(path, n, DIR "/%s", name);
}

int fontstore_choose(int role, const char *name)
{
  char cur[NAME_MAX_U], out[FONT_ROLES * (NAME_MAX_U + 8)];
  if (role < 0 || role >= FONT_ROLES || !is_font_name(name))
    return -1;
  int o = 0;
  for (int r = 0; r < FONT_ROLES; r++) {
    if (r == role)
      strcpy(cur, name);
    else
      fontstore_current(r, cur);
    o += snprintf(out + o, sizeof out - (unsigned)o, "%s%s\n", key[r], cur);
  }
  return writefile(CONF, out, o);
}

static int ieq(const char *a, const char *b)
{
  for (; *a && *b; a++, b++)
    if (((*a | 32) != (*b | 32)))
      return 0;
  return !*a && !*b;
}

#define FAMILIES 32
static struct {
  char family[64], name[NAME_MAX_U];
  int weight, italic;
} fam_cache[FAMILIES];
static int fam_n = -1;

static void scan_families(void)
{
  char name[NAME_MAX_U], p[96];
  fam_n = 0;
  for (int i = 0; fam_n < FAMILIES && fontstore_list(i, name) == 0; i++) {
    snprintf(p, sizeof p, DIR "/%s", name);
    int len;
    unsigned char *buf = slurp(p, &len);
    if (!buf)
      continue;
    struct font *f = font_load(buf, len);
    if (f && font_family(f, fam_cache[fam_n].family, sizeof fam_cache[0].family)) {
      strcpy(fam_cache[fam_n].name, name);
      font_style(f, &fam_cache[fam_n].weight, &fam_cache[fam_n].italic);
      fam_n++;
    }
    if (f)
      font_free(f);
    free(buf);
  }
}

int fontstore_find(const char *family, char *path, int n)
{
  if (fam_n < 0)
    scan_families();
  for (int i = 0; i < fam_n; i++)
    if (ieq(fam_cache[i].family, family)) {
      snprintf(path, n, DIR "/%s", fam_cache[i].name);
      return 1;
    }
  return 0;
}

int fontstore_match(const char *family, int weight, int italic, char *path, int n, int *got_weight, int *got_italic)
{
  if (fam_n < 0)
    scan_families();
  int best = -1, best_cost = 0;
  for (int i = 0; i < fam_n; i++) {
    if (!ieq(fam_cache[i].family, family))
      continue;
    int d = fam_cache[i].weight - weight;
    /* A slant mismatch outweighs any weight distance; at equal distance the heavier face wins for bold */
    int cost = (d < 0 ? -d : d) * 2 + (d < 0) * (weight >= 500) + (d > 0) * (weight < 500) +
               ((fam_cache[i].italic != 0) != (italic != 0)) * 10000;
    if (best < 0 || cost < best_cost) {
      best = i;
      best_cost = cost;
    }
  }
  if (best < 0)
    return 0;
  snprintf(path, n, DIR "/%s", fam_cache[best].name);
  if (got_weight)
    *got_weight = fam_cache[best].weight;
  if (got_italic)
    *got_italic = fam_cache[best].italic;
  return 1;
}
