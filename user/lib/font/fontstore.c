#include "fontstore.h"
#include "font.h"
#include "owos.h"

#define DIR "/etc/fonts"
#define CONF "/etc/fonts.conf"
#define NAME_MAX_U 64

#define FACES 48

static const char *const key[FONT_ROLES] = {"sans=", "mono=", "emoji=", "cjk="};

/* The installed faces, sorted by file name; nfaces < 0 until the first scan */
static struct face {
  char name[NAME_MAX_U];
  struct font_info info;
} faces[FACES];
static int nfaces = -1;

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
    nfaces = -1;
  }
  free(buf);
  return rc;
}

static int read_file(void *path, unsigned off, void *buf, unsigned len) { return readfile_at(path, off, buf, len); }

static void scan_faces(void)
{
  struct dirent_u de;
  char path[96];
  nfaces = 0;
  for (int idx = 0; nfaces < FACES && readdir(DIR, idx, &de) == 1; idx++) {
    struct font_info info;
    snprintf(path, sizeof path, DIR "/%s", de.name);
    if (de.is_dir || !is_font_name(de.name) || !font_probe(read_file, path, &info))
      continue;
    int at = nfaces++;
    for (; at > 0 && strcmp(faces[at - 1].name, de.name) > 0; at--)
      faces[at] = faces[at - 1];
    strcpy(faces[at].name, de.name);
    faces[at].info = info;
  }
}

static int face_role(const struct font_info *fi)
{
  return fi->color ? FONT_EMOJI : fi->cjk ? FONT_CJK : fi->mono ? FONT_MONO : FONT_SANS;
}

static int is_text_face(const struct face *f)
{
  int role = face_role(&f->info);
  return role == FONT_SANS || role == FONT_MONO;
}

int fontstore_list(int i, char *name)
{
  /* Listing from the start rescans, so a font another program installed shows up */
  if (i == 0 || nfaces < 0)
    scan_faces();
  for (int f = 0, seen = 0; f < nfaces; f++)
    if (is_text_face(&faces[f]) && seen++ == i) {
      strcpy(name, faces[f].name);
      return 0;
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

static int ieq(const char *a, const char *b)
{
  for (; *a && *b; a++, b++)
    if (((*a | 32) != (*b | 32)))
      return 0;
  return !*a && !*b;
}

/* How far a face is from the asked weight and slant; lower is closer */
static int face_cost(const struct font_info *fi, int weight, int italic)
{
  int d = fi->weight - weight;
  /* A slant mismatch outweighs any weight distance; at equal distance the heavier face wins for bold */
  return (d < 0 ? -d : d) * 2 + (d < 0) * (weight >= 500) + (d > 0) * (weight < 500) +
         ((fi->italic != 0) != (italic != 0)) * 10000;
}

/* The default face for `role`: of the faces suited to it, the family with the most installed faces (the first by
 * file name on a tie), so bold and italic text get real faces; its upright regular face. -1 if none suits. */
static int default_face(int role)
{
  int best = -1, best_count = 0;
  for (int i = 0; i < nfaces; i++) {
    if (face_role(&faces[i].info) != role)
      continue;
    int count = 0;
    for (int j = 0; j < nfaces; j++)
      count += face_role(&faces[j].info) == role && ieq(faces[j].info.family, faces[i].info.family);
    if (count > best_count) {
      best = i;
      best_count = count;
    }
  }
  for (int i = 0; best >= 0 && i < nfaces; i++)
    if (face_role(&faces[i].info) == role && ieq(faces[i].info.family, faces[best].info.family) &&
        face_cost(&faces[i].info, 400, 0) < face_cost(&faces[best].info, 400, 0))
      best = i;
  return best;
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
  if (nfaces < 0)
    scan_faces();
  int f = default_face(role);
  /* Sans and mono stand in for each other, so text still draws with only one of them installed */
  if (f < 0 && (role == FONT_SANS || role == FONT_MONO))
    f = default_face(role == FONT_SANS ? FONT_MONO : FONT_SANS);
  if (f >= 0)
    strcpy(name, faces[f].name);
  else
    name[0] = '\0';
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

int fontstore_find(const char *family, char *path, int n)
{
  if (nfaces < 0)
    scan_faces();
  for (int i = 0; i < nfaces; i++)
    if (is_text_face(&faces[i]) && faces[i].info.family[0] && ieq(faces[i].info.family, family)) {
      snprintf(path, n, DIR "/%s", faces[i].name);
      return 1;
    }
  return 0;
}

int fontstore_match(const char *family, int weight, int italic, char *path, int n, int *got_weight, int *got_italic)
{
  if (nfaces < 0)
    scan_faces();
  int best = -1, best_cost = 0;
  for (int i = 0; i < nfaces; i++) {
    if (!is_text_face(&faces[i]) || !faces[i].info.family[0] || !ieq(faces[i].info.family, family))
      continue;
    int cost = face_cost(&faces[i].info, weight, italic);
    if (best < 0 || cost < best_cost) {
      best = i;
      best_cost = cost;
    }
  }
  if (best < 0)
    return 0;
  snprintf(path, n, DIR "/%s", faces[best].name);
  if (got_weight)
    *got_weight = faces[best].info.weight;
  if (got_italic)
    *got_italic = faces[best].info.italic;
  return 1;
}
