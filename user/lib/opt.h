/* opt.h: command-line option parser (getopt_long style, no globals, no I/O).
 *
 *   static const struct opt opts[] = {
 *       {'c', "count", OPT_ARG, 'c'},
 *       {'v', "verbose", OPT_FLAG, 'v'},
 *   };
 *   struct optstate st;
 *   opt_init(&st, argc, argv);
 *   for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
 *     if (o == OPT_POS) host = st.arg;
 *     else if (o == OPT_BAD) usage(st.bad);
 *     else if (o == 'c') count = st.arg;
 *   }
 *
 * Accepts -x, -xVALUE, -x VALUE, clustered flags (-abc), --name, --name=VALUE,
 * --name VALUE, and "--" to end options. A lone "-" is a positional. Options
 * and positionals may be interleaved. */
#ifndef OWOS_OPT_H
#define OWOS_OPT_H

#define OPT_FLAG 0 /* takes no value  */
#define OPT_ARG 1  /* requires a value */

#define OPT_DONE (-1) /* no more arguments                              */
#define OPT_POS (-2)  /* positional argument in optstate.arg            */
#define OPT_BAD (-3)  /* unknown option or missing value; see .bad      */

struct opt {
  char sh;         /* short name, or 0                       */
  const char *lng; /* long name without "--", or NULL        */
  int has_arg;     /* OPT_FLAG / OPT_ARG                     */
  int id;          /* value opt_next returns for this option */
};

struct optstate {
  int argc;
  char **argv;
  int i;           /* next argv index                        */
  int pos;         /* offset inside a short-flag cluster     */
  int rest;        /* "--" seen: everything is positional    */
  const char *arg; /* option value, or the positional        */
  const char *bad; /* offending argument after OPT_BAD       */
};

#define OPT_N(tbl) ((int)(sizeof(tbl) / sizeof((tbl)[0])))

void opt_init(struct optstate *s, int argc, char **argv);
int opt_next(struct optstate *s, const struct opt *tbl, int n);

/* Strict unsigned decimal: digits only, no overflow. Returns 0 or -1. */
int opt_uint(const char *s, unsigned *out);

#endif
