/* stun.c - print this host's public (server-reflexive) address via STUN */

#include "stun.h"
#include "opt.h"
#include "owos.h"

int main(int argc, char **argv)
{
  const char *server = "stun.l.google.com";
  int port = 19302; /* Google's public STUN */

  static const struct opt opts[] = {{'h', "help", OPT_FLAG, 'h'}};
  struct optstate st;
  int npos = 0;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    unsigned p;
    if (o == 'h') {
      puts("usage: stun [server] [port]   (default stun.l.google.com 19302)\n");
      return 0;
    }
    if (o != OPT_POS) {
      printf("stun: unknown option %s\n", st.bad);
      return 1;
    }
    if (npos++ == 0) {
      server = st.arg;
      port = 3478; /* the IANA default port */
    } else if (npos == 2 && opt_uint(st.arg, &p) == 0 && p > 0 && p < 65536) {
      port = (int)p;
    }
  }

  stun_addr a;
  int r = stun_query(server, port, &a);
  if (r == STUN_EINTR)
    return EXIT_SIGINT;
  if (r != 1) {
    printf("stun: no response from %s:%u\n", server, (unsigned)port);
    return 1;
  }
  printf("server:  %s:%u\n", server, (unsigned)port);
  printf("public:  %u.%u.%u.%u:%u\n", a.ip[0], a.ip[1], a.ip[2], a.ip[3], a.port);
  return 0;
}
