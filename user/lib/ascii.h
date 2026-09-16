#ifndef OWOS_ASCII_H
#define OWOS_ASCII_H

static inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static inline int starts_ci(const char *s, const char *pre)
{
  while (*pre) {
    if (lower(*s) != lower(*pre))
      return 0;
    s++;
    pre++;
  }
  return 1;
}

static inline int streq_ci(const char *a, const char *b)
{
  while (*a && lower(*a) == lower(*b)) {
    a++;
    b++;
  }
  return lower(*a) == lower(*b);
}

static inline int eq_ci_n(const char *a, const char *b, int n)
{
  for (int i = 0; i < n; i++)
    if (lower(a[i]) != lower(b[i]))
      return 0;
  return 1;
}

static inline int contains_ci(const char *hay, const char *needle)
{
  for (; *hay; hay++)
    if (starts_ci(hay, needle))
      return 1;
  return 0;
}

#endif
