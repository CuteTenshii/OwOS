/* desktop.c: Desktop Entry parser, Exec expansion and directory scanner. */
#include "desktop.h"
#include "ascii.h"
#include "owos.h"

#define DESKTOP_SUFFIX ".desktop"
#define DESKTOP_SUFFIX_LEN 8
#define ACTION_GROUP "Desktop Action "

static char *dup_n(const char *s, int n)
{
  char *d = malloc((unsigned)n + 1);
  if (!d)
    return NULL;
  memcpy(d, s, (unsigned)n);
  d[n] = '\0';
  return d;
}

static char *dup_str(const char *s) { return dup_n(s, strlen(s)); }

static bool is_space(char c) { return c == ' ' || c == '\t'; }

static bool strv_push(struct desktop_strv *l, char *s)
{
  char **v = realloc(l->v, (unsigned)(l->n + 1) * sizeof *v);
  if (!v) {
    free(s);
    return false;
  }
  l->v = v;
  v[l->n++] = s;
  return true;
}

static void strv_free(struct desktop_strv *l)
{
  for (int i = 0; i < l->n; i++)
    free(l->v[i]);
  free(l->v);
}

static bool strv_has(const struct desktop_strv *l, const char *s)
{
  for (int i = 0; i < l->n; i++)
    if (streq(l->v[i], s))
      return true;
  return false;
}

static char *unescape(const char *s, int n)
{
  char *d = malloc((unsigned)n + 1);
  if (!d)
    return NULL;
  int j = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] != '\\' || i + 1 >= n) {
      d[j++] = s[i];
      continue;
    }
    char c = s[++i];
    switch (c) {
    case 's':
      d[j++] = ' ';
      break;
    case 'n':
      d[j++] = '\n';
      break;
    case 't':
      d[j++] = '\t';
      break;
    case 'r':
      d[j++] = '\r';
      break;
    case '\\':
    case ';':
      d[j++] = c;
      break;
    default:
      d[j++] = '\\';
      d[j++] = c;
    }
  }
  d[j] = '\0';
  return d;
}

static bool parse_list(const char *s, int n, struct desktop_strv *out)
{
  int start = 0;
  for (int i = 0; i <= n; i++) {
    if (i < n && s[i] == '\\' && i + 1 < n) {
      i++;
      continue;
    }
    if (i < n && s[i] != ';')
      continue;
    if (i > start) {
      char *item = unescape(s + start, i - start);
      if (!item || !strv_push(out, item))
        return false;
    }
    start = i + 1;
  }
  return true;
}

static int parse_int(const char *s, int n)
{
  int i = 0;
  bool neg = n > 0 && s[0] == '-';
  long v = 0;
  if (neg)
    i++;
  if (i >= n)
    return DESKTOP_ORDER_DEFAULT;
  for (; i < n; i++) {
    if (s[i] < '0' || s[i] > '9')
      return DESKTOP_ORDER_DEFAULT;
    if (v < 1000000000)
      v = v * 10 + (s[i] - '0');
  }
  return (int)(neg ? -v : v);
}

enum { K_STR, K_BOOL, K_INT, K_LIST };

struct key {
  const char *name;
  int kind;
  unsigned off;
};

#define ENTRY_KEY(n, k, f) {n, k, (unsigned)__builtin_offsetof(struct desktop_entry, f)}
static const struct key entry_keys[] = {
    ENTRY_KEY("Type", K_STR, type),
    ENTRY_KEY("Name", K_STR, name),
    ENTRY_KEY("GenericName", K_STR, generic_name),
    ENTRY_KEY("Comment", K_STR, comment),
    ENTRY_KEY("Exec", K_STR, exec),
    ENTRY_KEY("TryExec", K_STR, try_exec),
    ENTRY_KEY("Path", K_STR, path),
    ENTRY_KEY("Icon", K_STR, icon),
    ENTRY_KEY("StartupWMClass", K_STR, wm_class),
    ENTRY_KEY("Terminal", K_BOOL, terminal),
    ENTRY_KEY("NoDisplay", K_BOOL, no_display),
    ENTRY_KEY("Hidden", K_BOOL, hidden),
    ENTRY_KEY("X-OwOS-Order", K_INT, order),
    ENTRY_KEY("Categories", K_LIST, categories),
    ENTRY_KEY("Keywords", K_LIST, keywords),
    ENTRY_KEY("MimeType", K_LIST, mime_types),
    ENTRY_KEY("OnlyShowIn", K_LIST, only_show_in),
    ENTRY_KEY("NotShowIn", K_LIST, not_show_in),
    ENTRY_KEY("Actions", K_LIST, action_ids),
};

#define ACTION_KEY(n, f) {n, K_STR, (unsigned)__builtin_offsetof(struct desktop_action, f)}
static const struct key action_keys[] = {
    ACTION_KEY("Name", name),
    ACTION_KEY("Exec", exec),
    ACTION_KEY("Icon", icon),
};

static bool assign(void *base, const struct key *k, const char *raw, int n)
{
  char *p = (char *)base + k->off;
  switch (k->kind) {
  case K_STR:
    *(char **)p = unescape(raw, n);
    return *(char **)p != NULL;
  case K_BOOL:
    *(bool *)p = n == 4 && memcmp(raw, "true", 4) == 0;
    return true;
  case K_INT:
    *(int *)p = parse_int(raw, n);
    return true;
  default:
    return parse_list(raw, n, (struct desktop_strv *)p);
  }
}

static const struct key *find_key(const struct key *keys, int nkeys, const char *s, int n)
{
  for (int i = 0; i < nkeys; i++)
    if ((int)strlen(keys[i].name) == n && memcmp(keys[i].name, s, (unsigned)n) == 0)
      return &keys[i];
  return NULL;
}

static void action_free(struct desktop_action *a)
{
  free(a->id);
  free(a->name);
  free(a->exec);
  free(a->icon);
}

void desktop_free(struct desktop_entry *e)
{
  if (!e)
    return;
  free(e->id);
  free(e->location);
  free(e->name);
  free(e->generic_name);
  free(e->comment);
  free(e->exec);
  free(e->try_exec);
  free(e->path);
  free(e->icon);
  free(e->wm_class);
  free(e->type);
  strv_free(&e->categories);
  strv_free(&e->keywords);
  strv_free(&e->mime_types);
  strv_free(&e->only_show_in);
  strv_free(&e->not_show_in);
  strv_free(&e->action_ids);
  for (int i = 0; i < e->nactions; i++)
    action_free(&e->actions[i]);
  free(e->actions);
  free(e);
}

enum { G_NONE, G_ENTRY, G_ACTION };

static void keep_listed_actions(struct desktop_entry *e)
{
  int j = 0;
  for (int i = 0; i < e->nactions; i++) {
    if (e->actions[i].name && strv_has(&e->action_ids, e->actions[i].id))
      e->actions[j++] = e->actions[i];
    else
      action_free(&e->actions[i]);
  }
  e->nactions = j;
}

/* Index of the new action, -1 on OOM, -2 when the id already exists. */
static int add_action(struct desktop_entry *e, const char *id, int n)
{
  for (int i = 0; i < e->nactions; i++)
    if ((int)strlen(e->actions[i].id) == n && memcmp(e->actions[i].id, id, (unsigned)n) == 0)
      return -2;
  struct desktop_action *v = realloc(e->actions, (unsigned)(e->nactions + 1) * sizeof *v);
  if (!v)
    return -1;
  e->actions = v;
  memset(&v[e->nactions], 0, sizeof *v);
  v[e->nactions].id = dup_n(id, n);
  if (!v[e->nactions].id)
    return -1;
  return e->nactions++;
}

struct desktop_entry *desktop_parse(const char *text, int len, const char *id)
{
  struct desktop_entry *e = calloc(1, sizeof *e);
  if (!e)
    return NULL;
  e->order = DESKTOP_ORDER_DEFAULT;
  e->id = dup_str(id ? id : "");
  if (!e->id)
    goto fail;

  int group = G_NONE, cur = -1;
  bool have_entry = false;
  unsigned seen_entry = 0, seen_action = 0;
  for (int pos = 0; pos < len;) {
    int end = pos;
    while (end < len && text[end] != '\n')
      end++;
    const char *l = text + pos;
    int n = end - pos;
    pos = end + 1;
    while (n > 0 && (is_space(l[n - 1]) || l[n - 1] == '\r'))
      n--;
    while (n > 0 && is_space(*l)) {
      l++;
      n--;
    }
    if (n == 0 || l[0] == '#')
      continue;
    if (l[0] == '[') {
      group = G_NONE;
      if (l[n - 1] != ']')
        continue;
      const char *g = l + 1;
      int gn = n - 2;
      if (gn == 13 && memcmp(g, "Desktop Entry", 13) == 0 && !have_entry) {
        have_entry = true;
        group = G_ENTRY;
      } else if (gn > (int)sizeof ACTION_GROUP - 1 && memcmp(g, ACTION_GROUP, sizeof ACTION_GROUP - 1) == 0) {
        const char *aid = g + sizeof ACTION_GROUP - 1;
        int an = gn - ((int)sizeof ACTION_GROUP - 1);
        cur = add_action(e, aid, an);
        if (cur == -1)
          goto fail;
        seen_action = 0;
        group = cur >= 0 ? G_ACTION : G_NONE;
      }
      continue;
    }
    if (group == G_NONE)
      continue;
    int eq = 0;
    while (eq < n && l[eq] != '=')
      eq++;
    if (eq == n)
      continue;
    int kn = eq;
    while (kn > 0 && is_space(l[kn - 1]))
      kn--;
    const char *v = l + eq + 1;
    int vn = n - eq - 1;
    while (vn > 0 && is_space(*v)) {
      v++;
      vn--;
    }
    const struct key *keys = group == G_ENTRY ? entry_keys : action_keys;
    int nkeys = group == G_ENTRY ? (int)(sizeof entry_keys / sizeof *entry_keys)
                                 : (int)(sizeof action_keys / sizeof *action_keys);
    const struct key *k = find_key(keys, nkeys, l, kn);
    if (!k)
      continue;
    unsigned *seen = group == G_ENTRY ? &seen_entry : &seen_action;
    unsigned bit = 1u << (k - keys);
    if (*seen & bit)
      continue;
    *seen |= bit;
    if (!assign(group == G_ENTRY ? (void *)e : (void *)&e->actions[cur], k, v, vn))
      goto fail;
  }

  if (!have_entry || e->hidden || !e->type || !streq(e->type, "Application") || !e->name || !e->name[0] || !e->exec ||
      !e->exec[0])
    goto fail;
  keep_listed_actions(e);
  return e;
fail:
  desktop_free(e);
  return NULL;
}

struct sbuf {
  char *p;
  int n, cap;
};

static bool sb_add(struct sbuf *b, char c)
{
  if (b->n + 2 > b->cap) {
    int cap = b->cap ? b->cap * 2 : 32;
    char *p = realloc(b->p, (unsigned)cap);
    if (!p)
      return false;
    b->p = p;
    b->cap = cap;
  }
  b->p[b->n++] = c;
  b->p[b->n] = '\0';
  return true;
}

static bool sb_addstr(struct sbuf *b, const char *s)
{
  for (; s && *s; s++)
    if (!sb_add(b, *s))
      return false;
  return true;
}

struct argvb {
  char **v;
  int n;
};

static bool av_push(struct argvb *a, char *s)
{
  if (!s)
    return false;
  char **v = realloc(a->v, (unsigned)(a->n + 2) * sizeof *v);
  if (!v) {
    free(s);
    return false;
  }
  a->v = v;
  v[a->n++] = s;
  v[a->n] = NULL;
  return true;
}

void desktop_argv_free(char **argv, int argc)
{
  for (int i = 0; i < argc; i++)
    free(argv[i]);
  free(argv);
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n'; }

static bool flush_arg(struct argvb *a, struct sbuf *b, bool has_code, bool has_lit)
{
  bool ok = true;
  if (b->n > 0 || has_lit || !has_code)
    ok = av_push(a, dup_n(b->p ? b->p : "", b->n));
  b->n = 0;
  if (b->p)
    b->p[0] = '\0';
  return ok;
}

static bool push_files(struct argvb *a, const char *const *files, int nfiles)
{
  for (int i = 0; i < nfiles; i++)
    if (!av_push(a, dup_str(files[i])))
      return false;
  return true;
}

int desktop_exec_argv(const struct desktop_entry *e, const struct desktop_action *action, const char *const *files,
                      int nfiles, char ***argv, int *argc)
{
  const char *s = action ? action->exec : e->exec;
  struct argvb a = {0};
  struct sbuf b = {0};
  bool inq = false, intok = false, has_code = false, has_lit = false;
  if (!s)
    return -1;
  while (*s) {
    char c = *s;
    if (!inq && is_ws(c)) {
      if (intok && !flush_arg(&a, &b, has_code, has_lit))
        goto fail;
      intok = has_code = has_lit = false;
      s++;
    } else if (c == '"') {
      inq = !inq;
      intok = has_lit = true;
      s++;
    } else if (c == '\\') {
      char next = s[1];
      if (!next)
        goto fail;
      bool special = next == '"' || next == '`' || next == '$' || next == '\\';
      if (inq && !special) {
        if (!sb_add(&b, '\\'))
          goto fail;
        s++;
      } else {
        if (!sb_add(&b, next))
          goto fail;
        s += 2;
      }
      intok = has_lit = true;
    } else if (c == '%') {
      char code = s[1];
      bool whole = code && !inq && !intok && (s[2] == '\0' || is_ws(s[2]));
      if (code == '%') {
        if (!sb_add(&b, '%'))
          goto fail;
        intok = has_lit = true;
      } else if (code == 'F' || code == 'U') {
        if (!whole || !push_files(&a, files, nfiles))
          goto fail;
      } else if (code == 'i') {
        if (!whole)
          goto fail;
        if (e->icon && e->icon[0] && (!av_push(&a, dup_str("--icon")) || !av_push(&a, dup_str(e->icon))))
          goto fail;
      } else if (code == 'f' || code == 'u') {
        if (nfiles > 0 && !sb_addstr(&b, files[0]))
          goto fail;
        intok = has_code = true;
      } else if (code == 'c') {
        if (!sb_addstr(&b, e->name))
          goto fail;
        intok = has_code = true;
      } else if (code == 'k') {
        if (!sb_addstr(&b, e->location))
          goto fail;
        intok = has_code = true;
      } else if (code == 'd' || code == 'D' || code == 'n' || code == 'N' || code == 'v' || code == 'm') {
        intok = has_code = true;
      } else {
        goto fail;
      }
      s += 2;
    } else {
      if (!sb_add(&b, c))
        goto fail;
      intok = has_lit = true;
      s++;
    }
  }
  if (inq || (intok && !flush_arg(&a, &b, has_code, has_lit)) || a.n == 0)
    goto fail;
  free(b.p);
  *argv = a.v;
  *argc = a.n;
  return 0;
fail:
  free(b.p);
  desktop_argv_free(a.v, a.n);
  return -1;
}

static bool path_exists(const char *p)
{
  struct statinfo st;
  return stat(p, &st) == 0;
}

static bool try_exec_ok(const char *t)
{
  if (t[0] == '/')
    return path_exists(t);
  char *p = malloc((unsigned)strlen(t) + 6);
  if (!p)
    return false;
  strcpy(p, "/bin/");
  strcpy(p + 5, t);
  bool ok = path_exists(p);
  free(p);
  return ok;
}

bool desktop_visible(const struct desktop_entry *e)
{
  if (e->hidden || e->no_display)
    return false;
  if (e->only_show_in.n && !strv_has(&e->only_show_in, DESKTOP_ENVIRONMENT))
    return false;
  if (strv_has(&e->not_show_in, DESKTOP_ENVIRONMENT))
    return false;
  return !e->try_exec || !e->try_exec[0] || try_exec_ok(e->try_exec);
}

static struct desktop_entry *load_file(const char *dir, const char *name, int idlen)
{
  int dl = strlen(dir), nl = strlen(name);
  char *path = malloc((unsigned)(dl + nl + 2));
  if (!path)
    return NULL;
  memcpy(path, dir, (unsigned)dl);
  int j = dl;
  if (dl == 0 || dir[dl - 1] != '/')
    path[j++] = '/';
  memcpy(path + j, name, (unsigned)nl + 1);

  struct desktop_entry *e = NULL;
  struct statinfo st;
  if (stat(path, &st) == 0 && st.size > 0) {
    char *buf = malloc(st.size + 1);
    if (buf) {
      int got = readfile(path, buf, (int)st.size);
      char *id = dup_n(name, idlen);
      if (got > 0 && id)
        e = desktop_parse(buf, got, id);
      free(id);
      free(buf);
    }
  }
  if (e) {
    e->location = path;
  } else {
    free(path);
  }
  return e;
}

static int list_put(struct desktop_list *l, struct desktop_entry *e)
{
  for (int i = 0; i < l->n; i++) {
    if (streq(l->v[i]->id, e->id)) {
      desktop_free(l->v[i]);
      l->v[i] = e;
      return 0;
    }
  }
  struct desktop_entry **v = realloc(l->v, (unsigned)(l->n + 1) * sizeof *v);
  if (!v)
    return -1;
  l->v = v;
  v[l->n++] = e;
  return 0;
}

int desktop_scan(const char *dir, struct desktop_list *out)
{
  struct dirent_u de;
  int added = 0;
  for (int i = 0; readdir(dir, i, &de) == 1; i++) {
    int nl = strlen(de.name);
    if (de.is_dir || nl <= DESKTOP_SUFFIX_LEN || !streq(de.name + nl - DESKTOP_SUFFIX_LEN, DESKTOP_SUFFIX))
      continue;
    struct desktop_entry *e = load_file(dir, de.name, nl - DESKTOP_SUFFIX_LEN);
    if (!e)
      continue;
    if (list_put(out, e) < 0) {
      desktop_free(e);
      return -1;
    }
    added++;
  }
  return added;
}

static int name_cmp(const char *a, const char *b)
{
  while (*a && lower(*a) == lower(*b)) {
    a++;
    b++;
  }
  return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}

static bool entry_before(const struct desktop_entry *a, const struct desktop_entry *b)
{
  if (a->order != b->order)
    return a->order < b->order;
  return name_cmp(a->name, b->name) < 0;
}

void desktop_sort(struct desktop_list *l)
{
  for (int i = 1; i < l->n; i++) {
    struct desktop_entry *x = l->v[i];
    int j = i;
    for (; j > 0 && entry_before(x, l->v[j - 1]); j--)
      l->v[j] = l->v[j - 1];
    l->v[j] = x;
  }
}

void desktop_list_free(struct desktop_list *l)
{
  for (int i = 0; i < l->n; i++)
    desktop_free(l->v[i]);
  free(l->v);
  l->v = NULL;
  l->n = 0;
}
