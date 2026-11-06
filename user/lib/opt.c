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
