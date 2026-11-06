/* base64.c - encode or decode base64 (RFC 4648).
 *
 *   base64 [file]        encode <file> (or stdin) to base64, wrapped at 76 cols
 *   base64 -d [file]     decode base64 <file> (or stdin) to raw bytes
 *
 * Input is a file argument, or stdin when none is given or it is "-", so it
 * works in a pipe. The codec lives in user/lib/base64; this is just the front
 * end. */
#include "base64.h"
#include "opt.h"
#include "owos.h"
#include <stdbool.h>

/* Read the whole of `path` into a malloc'd buffer. NULL on error, *out_len set.
 */
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

static void usage(void)
{
  puts("usage:\n");
  puts("  base64 [file]      encode file (or stdin) to base64\n");
  puts("  base64 -d [file]   decode base64 file (or stdin)\n");
}

int main(int argc, char **argv)
{
  bool decode = false;
  const char *path = 0;
  char *input;
  int in_len = 0;

  static const struct opt opts[] = {{'d', "decode", OPT_FLAG, 'd'}, {'h', "help", OPT_FLAG, 'h'}};
  struct optstate st;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (o == 'd') {
      decode = true;
    } else if (o == 'h') {
      usage();
      return 0;
    } else if (o == OPT_POS) {
      path = streq(st.arg, "-") ? 0 : st.arg; /* "-" = stdin */
    } else {
      printf("base64: unknown option %s\n", st.bad);
      return 1;
    }
  }

  if (path) {
    input = read_file(path, &in_len);
    if (!input) {
      printf("base64: cannot read %s\n", path);
      return 1;
    }
  } else {
    input = read_stdin(&in_len);
    if (!input) {
      printf("base64: out of memory\n");
      return 1;
    }
  }

  if (decode) {
    int cap = base64_decoded_cap(in_len);
    unsigned char *out = malloc(cap ? cap : 1);
    if (!out) {
      free(input);
      printf("base64: out of memory\n");
      return 1;
    }
    int n = base64_decode(input, in_len, out);
    if (n < 0) {
      free(out);
      free(input);
      printf("base64: invalid input\n");
      return 1;
    }
    write(1, out, n);
    free(out);
  } else {
    int cap = base64_encoded_len(in_len);
    char *out = malloc(cap + 1);
    if (!out) {
      free(input);
      printf("base64: out of memory\n");
      return 1;
    }
    base64_encode((const unsigned char *)input, in_len, out);
    for (int off = 0; off < cap; off += 76) { /* 76-col lines, like coreutils */
      int n = cap - off < 76 ? cap - off : 76;
      write(1, out + off, n);
      write(1, "\n", 1);
    }
    free(out);
  }
  free(input);
  return 0;
}
