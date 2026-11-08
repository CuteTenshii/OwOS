/* opt.h: command-line option parser (getopt_long style, no globals, no I/O) */
#ifndef OWOS_OPT_H
#define OWOS_OPT_H

#define OPT_FLAG 0 /* takes no value */
#define OPT_ARG 1  /* requires a value */

#define OPT_DONE (-1) /* no more arguments */
#define OPT_POS (-2)  /* positional argument in optstate.arg */
#define OPT_BAD (-3)  /* unknown option or missing value; see .bad */

struct opt {
  char sh;          /* short name, or 0 */
  const char *lng;  /* long name without "--", or NULL */
  int has_arg;      /* OPT_FLAG / OPT_ARG */
  int id;           /* value opt_next returns for this option */
  const char *meta; /* value placeholder shown for OPT_ARG, e.g. "FILE" (default "ARG") */
  const char *help; /* one-line description for opt_help */
};

struct optstate {
  int argc;
  char **argv;
  int i;           /* next argv index */
  int pos;         /* offset inside a short-flag cluster */
  int rest;        /* "--" seen: everything is positional */
  const char *arg; /* option value, or the positional */
  const char *bad; /* offending argument after OPT_BAD */
};

#define OPT_N(tbl) ((int)(sizeof(tbl) / sizeof((tbl)[0])))

void opt_init(struct optstate *s, int argc, char **argv);
int opt_next(struct optstate *s, const struct opt *tbl, int n);

/* Usage and help text generated from the option table. Both write into buf (at most cap bytes, always
 * NUL-terminated when cap > 0) and return the full length, like snprintf. `args` is the positional-argument
 * synopsis ("FILE..."), `summary` a sentence for opt_help. */
int opt_usage(char *buf, int cap, const char *prog, const char *args, const struct opt *tbl, int n);
int opt_help(char *buf, int cap, const char *prog, const char *args, const char *summary, const struct opt *tbl, int n);

/* Guest-side wrappers that print the text above to the console (defined in lib.c). */
void opt_print_usage(const char *prog, const char *args, const struct opt *tbl, int n);
void opt_print_help(const char *prog, const char *args, const char *summary, const struct opt *tbl, int n);

/* Strict unsigned decimal: digits only, no overflow */
int opt_uint(const char *s, unsigned *out);

#endif
