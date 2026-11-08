/* json.c - a JSON command-line tool: validate, pretty-print, or minify */
#include "json.h"
#include "opt.h"
#include "owos.h"
#include <stdbool.h>

/* Read the whole of `path` into a malloc'd buffer */
static char *read_file(const char *path, int *out_len)
{
  struct statinfo st;
  char *buf;
  unsigned off = 0;
  if (stat(path, &st) != 0)
    return 0;
  buf = malloc(st.size ? st.size : 1);
  if (!buf)
    return 0;
  while (off < st.size) {
    int n = readfile_at(path, off, buf + off, st.size - off);
    if (n <= 0)
      break;
    off += (unsigned)n;
  }
  if (off != st.size) {
    free(buf);
    return 0;
  }
  *out_len = (int)st.size;
  return buf;
}

/* Drain stdin (fd 0) into a growable malloc'd buffer */
static char *read_stdin(int *out_len)
{
  int cap = 4096, n = 0;
  char *b = malloc(cap);
  if (!b)
    return 0;
  for (;;) {
    int r;
    if (n == cap) {
      char *nb = realloc(b, (unsigned)(cap * 2));
      if (!nb) {
        free(b);
        return 0;
      }
      b = nb;
      cap *= 2;
    }
    r = read(0, b + n, cap - n);
    if (r <= 0)
      break;
    n += r;
  }
  *out_len = n;
  return b;
}

static int parse_int(const char *s, int defval)
{
  bool any = false;
  int v = 0;
  if (!s)
    return defval;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
    any = true;
  }
  return any ? v : defval;
}

int main(int argc, char **argv)
{
  bool validate = false;
  int indent = 2;
  const char *path = 0;
  char *input;
  int in_len = 0, out_len = 0;
  json_error err;
  json_value *root;
  char *out;

  static const struct opt opts[] = {
      {'c', "compact", OPT_FLAG, 'c', 0, "print on one line"},
      {'v', "validate", OPT_FLAG, 'v', 0, "only check that the input is valid"},
      {'i', "indent", OPT_ARG, 'i', "N", "indent with N spaces (default 2)"},
      {'h', "help", OPT_FLAG, 'h', 0, "show this help"},
  };
  struct optstate st;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (o == 'c') {
      indent = 0;
    } else if (o == 'v') {
      validate = true;
    } else if (o == 'i') {
      indent = parse_int(st.arg, 2);
    } else if (o == 'h') {
      opt_print_help("json", "[FILE]", "Pretty-print, minify or validate JSON from FILE (or stdin).", opts,
                     OPT_N(opts));
      return 0;
    } else if (o == OPT_POS) {
      if (!path)
        path = st.arg;
    } else {
      printf("json: unknown option %s\n", st.bad);
      opt_print_usage("json", "[FILE]", opts, OPT_N(opts));
      return 1;
    }
  }

  if (path) {
    input = read_file(path, &in_len);
    if (!input) {
      printf("json: cannot read %s\n", path);
      return 1;
    }
  } else {
    input = read_stdin(&in_len);
    if (!input) {
      printf("json: out of memory\n");
      return 1;
    }
  }

  root = json_parse(input, in_len, &err);
  free(input);
  if (!root) {
    printf("json: %s%sline %d, col %d: %s\n", path ? path : "", path ? ": " : "", err.line, err.col, err.msg);
    return 1;
  }

  if (validate) {
    json_free(root);
    puts("valid\n");
    return 0;
  }

  out = json_encode(root, indent, &out_len);
  json_free(root);
  if (!out) {
    printf("json: out of memory\n");
    return 1;
  }
  write(1, out, out_len);
  write(1, "\n", 1);
  free(out);
  return 0;
}
