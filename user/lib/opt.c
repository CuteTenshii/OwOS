#include "opt.h"

void opt_init(struct optstate *s, int argc, char **argv)
{
  s->argc = argc;
  s->argv = argv;
  s->i = 1;
  s->pos = 0;
  s->rest = 0;
  s->arg = 0;
  s->bad = 0;
}

static int same(const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int bad(struct optstate *s, const char *what)
{
  s->bad = what;
  return OPT_BAD;
}

static int next_long(struct optstate *s, const struct opt *tbl, int n)
{
  const char *a = s->argv[s->i++];
  const char *name = a + 2, *eq = name;
  while (*eq && *eq != '=')
    eq++;
  for (int k = 0; k < n; k++) {
    const char *l = tbl[k].lng;
    int m = 0;
    if (!l)
      continue;
    while (l[m] && l[m] == name[m] && name + m < eq)
      m++;
    if (l[m] || name + m != eq)
      continue;
    if (tbl[k].has_arg == OPT_FLAG)
      return *eq ? bad(s, a) : tbl[k].id;
    if (*eq) {
      s->arg = eq + 1;
    } else if (s->i < s->argc) {
      s->arg = s->argv[s->i++];
    } else {
      return bad(s, a);
    }
    return tbl[k].id;
  }
  return bad(s, a);
}

static int next_short(struct optstate *s, const struct opt *tbl, int n)
{
  const char *a = s->argv[s->i];
  char c = a[s->pos];
  const struct opt *o = 0;
  for (int k = 0; k < n && !o; k++)
    if (tbl[k].sh == c)
      o = &tbl[k];
  s->pos++;
  if (!o) {
    if (!a[s->pos]) {
      s->pos = 0;
      s->i++;
    }
    return bad(s, a);
  }
  if (o->has_arg == OPT_ARG) {
    if (a[s->pos]) {
      s->arg = a + s->pos;
    } else if (s->i + 1 < s->argc) {
      s->arg = s->argv[s->i + 1];
      s->i++;
    } else {
      s->pos = 0;
      s->i++;
      return bad(s, a);
    }
    s->pos = 0;
    s->i++;
    return o->id;
  }
  if (!a[s->pos]) {
    s->pos = 0;
    s->i++;
  }
  return o->id;
}

int opt_next(struct optstate *s, const struct opt *tbl, int n)
{
  if (s->pos == 0) {
    if (s->i >= s->argc)
      return OPT_DONE;
    const char *a = s->argv[s->i];
    if (s->rest || a[0] != '-' || !a[1]) {
      s->arg = a;
      s->i++;
      return OPT_POS;
    }
    if (same(a, "--")) {
      s->rest = 1;
      s->i++;
      return opt_next(s, tbl, n);
    }
    if (a[1] == '-')
      return next_long(s, tbl, n);
    s->pos = 1;
  }
  return next_short(s, tbl, n);
}

int opt_uint(const char *s, unsigned *out)
{
  unsigned v = 0;
  if (!*s)
    return -1;
  for (; *s; s++) {
    unsigned d = (unsigned)(*s - '0');
    if (d > 9 || v > (0xFFFFFFFFu - d) / 10)
      return -1;
    v = v * 10 + d;
  }
  *out = v;
  return 0;
}

struct out {
  char *buf;
  int cap, len;
};

static void put(struct out *o, const char *s)
{
  for (; *s; s++, o->len++)
    if (o->len + 1 < o->cap)
      o->buf[o->len] = *s;
}

static void putc_(struct out *o, char c)
{
  char s[2] = {c, 0};
  put(o, s);
}

static int finish(struct out *o)
{
  if (o->cap > 0)
    o->buf[o->len < o->cap ? o->len : o->cap - 1] = '\0';
  return o->len;
}

static int slen(const char *s)
{
  int n = 0;
  while (s[n])
    n++;
  return n;
}

static const char *meta_of(const struct opt *o) { return o->meta ? o->meta : "ARG"; }

static void usage_line(struct out *o, const char *prog, const char *args, const struct opt *tbl, int n)
{
  int clustered = 0;
  put(o, "usage: ");
  put(o, prog);
  for (int i = 0; i < n; i++)
    if (tbl[i].sh && tbl[i].has_arg == OPT_FLAG) {
      if (!clustered)
        put(o, " [-");
      putc_(o, tbl[i].sh);
      clustered = 1;
    }
  if (clustered)
    putc_(o, ']');
  for (int i = 0; i < n; i++) {
    const struct opt *t = &tbl[i];
    if (t->has_arg == OPT_FLAG && t->sh)
      continue;
    if (!t->sh && !t->lng)
      continue;
    put(o, " [");
    if (t->sh) {
      putc_(o, '-');
      putc_(o, t->sh);
    } else {
      put(o, "--");
      put(o, t->lng);
    }
    if (t->has_arg == OPT_ARG) {
      putc_(o, ' ');
      put(o, meta_of(t));
    }
    putc_(o, ']');
  }
  if (args && *args) {
    putc_(o, ' ');
    put(o, args);
  }
  putc_(o, '\n');
}

int opt_usage(char *buf, int cap, const char *prog, const char *args, const struct opt *tbl, int n)
{
  struct out o = {buf, cap, 0};
  usage_line(&o, prog, args, tbl, n);
  return finish(&o);
}

/* Width of "-o, --out META" for one row */
static int row_width(const struct opt *t)
{
  int w = 0;
  if (t->sh)
    w += 2;
  if (t->sh && t->lng)
    w += 2;
  if (t->lng)
    w += 2 + slen(t->lng);
  else if (!t->sh)
    return 0;
  if (t->has_arg == OPT_ARG)
    w += 1 + slen(meta_of(t));
  return w;
}

int opt_help(char *buf, int cap, const char *prog, const char *args, const char *summary, const struct opt *tbl, int n)
{
  struct out o = {buf, cap, 0};
  int width = 0;
  usage_line(&o, prog, args, tbl, n);
  if (summary && *summary) {
    putc_(&o, '\n');
    put(&o, summary);
    putc_(&o, '\n');
  }
  for (int i = 0; i < n; i++)
    if (row_width(&tbl[i]) > width)
      width = row_width(&tbl[i]);
  if (width)
    put(&o, "\noptions:\n");
  for (int i = 0; i < n; i++) {
    const struct opt *t = &tbl[i];
    int w = row_width(t);
    if (!w)
      continue;
    put(&o, "  ");
    if (t->sh) {
      putc_(&o, '-');
      putc_(&o, t->sh);
    }
    if (t->sh && t->lng)
      put(&o, ", ");
    if (t->lng) {
      put(&o, "--");
      put(&o, t->lng);
    }
    if (t->has_arg == OPT_ARG) {
      putc_(&o, ' ');
      put(&o, meta_of(t));
    }
    if (t->help && *t->help) {
      for (; w < width + 2; w++)
        putc_(&o, ' ');
      put(&o, t->help);
    }
    putc_(&o, '\n');
  }
  return finish(&o);
}
