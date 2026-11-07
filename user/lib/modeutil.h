/* modeutil.h: chmod mode parsing (octal and symbolic), header-only for host tests. */
#ifndef OWOS_MODEUTIL_H
#define OWOS_MODEUTIL_H

#include "../../kernel/perm.h"
#include <stdbool.h>

/* Parse "755" or "u+x,go-w,+X" against `cur`; false on bad input or setuid/setgid. */
static inline bool mode_parse(const char *s, unsigned cur, unsigned umask, bool is_dir, unsigned *out)
{
  if (!s || !*s)
    return false;
  if (*s >= '0' && *s <= '7') {
    unsigned v = 0;
    for (; *s; s++) {
      if (*s < '0' || *s > '7' || v > 0777u)
        return false;
      v = v * 8 + (unsigned)(*s - '0');
    }
    if (v & ~PERM_MODE_MASK)
      return false;
    *out = v;
    return true;
  }
  unsigned mode = cur & PERM_MODE_MASK;
  for (;;) {
    unsigned who = 0;
    bool explicit_who = false;
    for (;; s++) {
      if (*s == 'u')
        who |= 0700;
      else if (*s == 'g')
        who |= 0070;
      else if (*s == 'o')
        who |= 0007;
      else if (*s == 'a')
        who |= 0777;
      else
        break;
      explicit_who = true;
    }
    if (!explicit_who)
      who = 0777;
    if (*s != '+' && *s != '-' && *s != '=')
      return false;
    while (*s == '+' || *s == '-' || *s == '=') {
      char op = *s++;
      unsigned bits = 0;
      bool sticky = false;
      for (;; s++) {
        unsigned copy = 0;
        if (*s == 'r')
          bits |= 0444;
        else if (*s == 'w')
          bits |= 0222;
        else if (*s == 'x')
          bits |= 0111;
        else if (*s == 'X') {
          if (is_dir || (mode & 0111))
            bits |= 0111;
        } else if (*s == 't')
          sticky = true;
        else if (*s == 'u')
          copy = (mode >> 6) & 7u;
        else if (*s == 'g')
          copy = (mode >> 3) & 7u;
        else if (*s == 'o')
          copy = mode & 7u;
        else
          break;
        if (*s == 'u' || *s == 'g' || *s == 'o')
          bits |= copy | (copy << 3) | (copy << 6);
      }
      unsigned apply = bits & who;
      if (!explicit_who)
        apply &= ~umask;
      if (op == '+')
        mode |= apply;
      else if (op == '-')
        mode &= ~apply;
      else
        mode = (mode & ~who) | apply;
      if (sticky) {
        if (op == '-')
          mode &= ~PERM_STICKY;
        else
          mode |= PERM_STICKY;
      }
    }
    if (*s == '\0')
      break;
    if (*s != ',')
      return false;
    s++;
  }
  *out = mode & PERM_MODE_MASK;
  return true;
}

#endif
