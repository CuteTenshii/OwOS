/* stun.c — print this host's public (server-reflexive) address via STUN.
 *
 * A thin harness over user/lib/stun: send a Binding request to a STUN server
 * and report the XOR-MAPPED-ADDRESS the server saw — the IP:port a NAT maps our
 * UDP socket to. The same reflexive lookup ICE/WebRTC uses to gather
 * candidates.
 *
 *   stun                          query stun.l.google.com:19302
 *   stun stun.cloudflare.com      a different server, default port 3478
 *   stun 10.0.2.2 3478            explicit host + port
 */

#include "stun.h"
#include "owos.h"

int main(int argc, char **argv)
{
  const char *server = "stun.l.google.com";
  int port = 19302; /* Google's public STUN     */

  if (argc > 1) {
    if (streq(argv[1], "-h") || streq(argv[1], "--help")) {
      puts("usage: stun [server] [port]   (default stun.l.google.com 19302)\n");
      return 0;
    }
    server = argv[1];
    port = 3478; /* the IANA default port    */
  }
  if (argc > 2) {
    int p = 0;
    for (const char *s = argv[2]; *s >= '0' && *s <= '9'; s++)
      p = p * 10 + (*s - '0');
    if (p > 0 && p < 65536)
      port = p;
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
