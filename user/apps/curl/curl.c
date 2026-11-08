/* curl.c - an HTTP/1.1 client, a thin CLI over libcurl (user/lib/http.c) */

#include "http.h"
#include "opt.h"
#include "owos.h"
#include <stdbool.h>

/* body sinks */

/* Console mode: batch and write to fd 1 */
static char con_buf[1024];
static int con_len;

static void con_flush(void)
{
  if (con_len) {
    write(1, con_buf, con_len);
    con_len = 0;
  }
}

static int sink_console(void *ctx, const unsigned char *buf, int len)
{
  (void)ctx;
  for (int i = 0; i < len; i++) {
    if (con_len == (int)sizeof con_buf)
      con_flush();
    con_buf[con_len++] = (char)buf[i];
  }
  return 0;
}

/* -o mode: accumulate into a buffer, then writefile */
static char save_buf[256 * 1024];
static bool save_overflow;
static int save_len;

static int sink_save(void *ctx, const unsigned char *buf, int len)
{
  (void)ctx;
  for (int i = 0; i < len; i++) {
    if (save_len < (int)sizeof save_buf)
      save_buf[save_len++] = (char)buf[i];
    else
      save_overflow = true;
  }
  return 0;
}

int main(int argc, char **argv)
{
  bool show_headers = false;
  bool http2 = false;
  bool http3 = false;
  const char *method = "GET";
  const char *outname = 0;
  const char *url = 0;
  const char *user_agent = 0; /* -A: overrides the default UA */
  bool ua_in_headers = false; /* set if a -H line already gives one */
  char xhdr[2048];
  int xhlen = 0; /* -H request headers ("name: value\r\n"…) */

  static const struct opt opts[] = {
      {'i', 0, OPT_FLAG, 'i', 0, "print the response headers"},
      {0, "http2", OPT_FLAG, '2', 0, "use HTTP/2"},
      {'3', "http3", OPT_FLAG, '3', 0, "use HTTP/3"},
      {'X', 0, OPT_ARG, 'X', "METHOD", "request method (default GET)"},
      {'o', 0, OPT_ARG, 'o', "FILE", "save the body to FILE"},
      {'A', "user-agent", OPT_ARG, 'A', "UA", "User-Agent to send"},
      {'H', 0, OPT_ARG, 'H', "\"NAME: VALUE\"", "add a request header (repeatable)"},
      {'h', "help", OPT_FLAG, 'h', 0, "show this help"},
  };
  struct optstate st;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (o == 'h') {
      opt_print_help("curl", "URL", "Fetch URL over HTTP or HTTPS.", opts, OPT_N(opts));
      return 0;
    } else if (o == 'i') {
      show_headers = true;
    } else if (o == '2') {
      http2 = true;
    } else if (o == '3') {
      http3 = true;
    } else if (o == 'X') {
      method = st.arg;
    } else if (o == 'o') {
      outname = st.arg;
    } else if (o == 'A') {
      user_agent = st.arg;
    } else if (o == 'H') {
      const char *h = st.arg;
      if (starts_ci(h, "user-agent:"))
        ua_in_headers = true;
      for (int k = 0; h[k] && xhlen < (int)sizeof xhdr - 3; k++)
        xhdr[xhlen++] = h[k];
      xhdr[xhlen++] = '\r';
      xhdr[xhlen++] = '\n';
      xhdr[xhlen] = '\0';
    } else if (o == OPT_POS) {
      url = st.arg;
    } else {
      printf("curl: bad option %s\n", st.bad);
      url = 0;
      break;
    }
  }

  if (!url) {
    opt_print_usage("curl", "URL", opts, OPT_N(opts));
    return 1;
  }

  /* The User-Agent is the tool's to send, not libcurl's (as on a real system) */
  if (!ua_in_headers) {
    const char *ua = user_agent ? user_agent : "owos-curl/1.0";
    for (const char *s = "User-Agent: "; *s && xhlen < (int)sizeof xhdr - 3; s++)
      xhdr[xhlen++] = *s;
    for (const char *s = ua; *s && xhlen < (int)sizeof xhdr - 3; s++)
      xhdr[xhlen++] = *s;
    xhdr[xhlen++] = '\r';
    xhdr[xhlen++] = '\n';
    xhdr[xhlen] = '\0';
  }

  char hdr[4096];
  int hlen = 0;
  int rc = http_request_ex(method, url, http2, http3, xhlen ? xhdr : 0, show_headers ? hdr : 0, (int)sizeof hdr, &hlen,
                           outname ? sink_save : sink_console, 0);

  if (show_headers && hlen > 0)
    write(1, hdr, hlen);

  if (rc < 0) {
    con_flush();
    switch (rc) {
    case HTTP_EPARSE:
      puts("curl: could not parse URL\n");
      break;
    case HTTP_ERESOLVE:
      printf("curl: could not resolve host: %s\n", url);
      break;
    case HTTP_ECONNECT:
      puts("curl: failed to connect\n");
      break;
    case HTTP_ESEND:
      puts("curl: failed to send request\n");
      break;
    case HTTP_ENORESP:
      puts("curl: no response from server\n");
      break;
    case HTTP_EINTR:
      return EXIT_SIGINT; /* silent; the shell echoed ^C */
    default:
      puts("curl: transfer failed\n");
      break;
    }
    return 1;
  }

  if (outname) {
    if (save_overflow)
      puts("curl: response exceeds 256 KiB buffer, truncated\n");
    int wr = writefile(outname, save_buf, save_len);
    if (wr != 0) {
      printf("curl: failed to save %s%s\n", outname, wr == -EACCES ? " (permission denied)" : "");
      return 1;
    }
    printf("curl: saved %u bytes to %s\n", (unsigned)save_len, outname);
  } else {
    con_flush();
    puts("\n");
  }
  return 0;
}
