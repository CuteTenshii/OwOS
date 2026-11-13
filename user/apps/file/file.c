/* file.c - identify files by their content.
 *
 *   file [-b] [-i | --mime-type | --mime-encoding] FILE...
 *
 * "-" reads stdin. Detection lives in user/lib/mime/magic.c. */
#include "magic.h"
#include "opt.h"
#include "owos.h"
#include <stdbool.h>

enum { SHOW_DESC, SHOW_MIME, SHOW_MIME_TYPE, SHOW_MIME_ENCODING };
enum { OPT_MIME_TYPE = 1000, OPT_MIME_ENCODING };

static unsigned char head[MAGIC_PEEK];

/* Fill `head` from the start of `path`; bytes read, or the negative readfile_at error. */
static int read_head(const char *path, unsigned size, bool *complete)
{
  int got = 0;
  while (got < MAGIC_PEEK) {
    int n = readfile_at(path, (unsigned)got, head + got, MAGIC_PEEK - got);
    if (n < 0 && got == 0)
      return n;
    if (n <= 0)
      break;
    got += n;
  }
  *complete = got < MAGIC_PEEK || size == (unsigned)got;
  return got;
}

static int read_stdin_head(bool *complete)
{
  int got = 0;
  while (got < MAGIC_PEEK) {
    int n = read(0, head + got, MAGIC_PEEK - got);
    if (n <= 0)
      break;
    got += n;
  }
  *complete = got < MAGIC_PEEK;
  return got;
}

static void print_result(const char *name, int pad, bool brief, int show, const char *desc, const char *mime,
                         const char *charset)
{
  if (!brief) {
    printf("%s:", name);
    for (int i = 0; i <= pad; i++)
      write(1, " ", 1);
  }
  if (show == SHOW_MIME_TYPE)
    printf("%s\n", mime);
  else if (show == SHOW_MIME_ENCODING)
    printf("%s\n", charset);
  else if (show == SHOW_MIME)
    printf("%s; charset=%s\n", mime, charset);
  else
    printf("%s\n", desc);
}

/* Identify one operand; false when it could not be read. */
static bool identify(const char *arg, int pad, bool brief, int show)
{
  struct magic_info mi;
  bool complete = false;
  int n;

  if (streq(arg, "-")) {
    n = read_stdin_head(&complete);
    magic_identify(head, n, complete, &mi);
    print_result("/dev/stdin", pad, brief, show, mi.desc, mi.mime, mi.charset);
    return true;
  }

  struct statinfo st;
  int rc = stat(arg, &st);
  if (rc != 0) {
    print_result(arg, pad, brief, SHOW_DESC,
                 rc == -EACCES ? "cannot open (Permission denied)" : "cannot open (No such file or directory)", 0, 0);
    return false;
  }
  if (st.is_dir) {
    print_result(arg, pad, brief, show, "directory", "inode/directory", "binary");
    return true;
  }
  n = read_head(arg, st.size, &complete);
  if (n < 0) {
    print_result(arg, pad, brief, SHOW_DESC,
                 n == -EACCES ? "cannot open (Permission denied)" : "cannot open (read error)", 0, 0);
    return false;
  }
  magic_identify(head, n, complete, &mi);
  print_result(arg, pad, brief, show, mi.desc, mi.mime, mi.charset);
  return true;
}

int main(int argc, char **argv)
{
  bool brief = false;
  int show = SHOW_DESC, nfiles = 0, width = 0;

  static const struct opt opts[] = {
      {'b', "brief", OPT_FLAG, 'b', 0, "do not prepend file names"},
      {'i', "mime", OPT_FLAG, 'i', 0, "print the MIME type and charset"},
      {0, "mime-type", OPT_FLAG, OPT_MIME_TYPE, 0, "print only the MIME type"},
      {0, "mime-encoding", OPT_FLAG, OPT_MIME_ENCODING, 0, "print only the charset"},
      {'h', "help", OPT_FLAG, 'h', 0, "show this help"},
  };
  struct optstate st;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (o == 'b') {
      brief = true;
    } else if (o == 'i') {
      show = SHOW_MIME;
    } else if (o == OPT_MIME_TYPE) {
      show = SHOW_MIME_TYPE;
    } else if (o == OPT_MIME_ENCODING) {
      show = SHOW_MIME_ENCODING;
    } else if (o == 'h') {
      opt_print_help("file", "FILE...", "Determine the type of each FILE from its content.", opts, OPT_N(opts));
      return 0;
    } else if (o == OPT_POS) {
      int len = streq(st.arg, "-") ? (int)strlen("/dev/stdin") : (int)strlen(st.arg);
      if (len > width)
        width = len;
      nfiles++;
    } else {
      printf("file: unknown option %s\n", st.bad);
      opt_print_usage("file", "FILE...", opts, OPT_N(opts));
      return 1;
    }
  }
  if (!nfiles) {
    opt_print_usage("file", "FILE...", opts, OPT_N(opts));
    return 1;
  }

  /* Second pass over the operands now that the name column width is known. */
  bool ok = true;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (o != OPT_POS)
      continue;
    int len = streq(st.arg, "-") ? (int)strlen("/dev/stdin") : (int)strlen(st.arg);
    if (!identify(st.arg, width - len, brief, show))
      ok = false;
  }
  return ok ? 0 : 1;
}
