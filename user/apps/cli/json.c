/* json.c — a JSON command-line tool: validate, pretty-print, or minify.
 *
 *   json [file]        pretty-print <file> (or stdin) with 2-space indent
 *   json -c [file]     compact (minify) instead
 *   json -i N [file]   pretty-print with N spaces of indent
 *   json -v [file]     validate only: print "valid" or the parse error
 *
 * The parsing/encoding lives in user/lib/json; this is just the front end.
 * Input is a file argument, or stdin when none is given (so it works in a
 * pipe). */
#include "json.h"
#include "owos.h"

/* Read the whole of `path` into a malloc'd buffer (NUL is not appended; the
 * parser is given an explicit length). NULL on error, with *out_len set. */
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

/* Drain stdin (fd 0) into a growable malloc'd buffer. NULL on OOM. */
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
  int v = 0, any = 0;
  if (!s)
    return defval;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
    any = 1;
  }
  return any ? v : defval;
}

static void usage(void)
{
  puts("usage:\n");
  puts("  json [file]        pretty-print JSON (2-space indent)\n");
  puts("  json -c [file]     compact / minify\n");
  puts("  json -i N [file]   pretty-print with N spaces\n");
  puts("  json -v [file]     validate only\n");
  puts("input is <file>, or stdin when no file is given\n");
}

int main(int argc, char **argv)
{
  int indent = 2, validate = 0, i;
  const char *path = 0;
  char *input;
  int in_len = 0, out_len = 0;
  json_error err;
  json_value *root;
  char *out;

  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' && a[1] && !path) {
      if (a[1] == 'c')
        indent = 0;
      else if (a[1] == 'v')
        validate = 1;
      else if (a[1] == 'i')
        indent = parse_int(argv[++i], 2);
      else if (a[1] == 'h') {
        usage();
        return 0;
      } else {
        printf("json: unknown option %s\n", a);
        return 1;
      }
    } else if (!path) {
      path = a;
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
