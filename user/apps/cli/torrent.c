/* torrent.c — a BitTorrent client, a thin CLI over libtorrent
 * (user/lib/net/{bencode,torrent,tracker,peer}.c + user/lib/crypto).
 *
 *   torrent info  <file>            parse a .torrent: name, infohash, layout
 *   torrent test                    offline self-check (parse + infohash)
 *   torrent peers <file>            announce to the tracker, list peers
 *   torrent peer  <file> <ip> <port>  fetch + verify piece 0 from one peer
 *   torrent get   <file>            download a single-file torrent to owofs
 */

#include "torrent.h"
#include "owos.h"

/* Read a whole .torrent into a static buffer; returns length or -1. */
static unsigned char tbuf[1u << 20]; /* 1 MiB: plenty for a metainfo file */

static int load(const char *path, struct torrent *t)
{
  int n = readfile(path, (char *)tbuf, (int)sizeof tbuf);
  if (n <= 0) {
    puts("torrent: cannot read ");
    puts(path);
    puts("\n");
    return -1;
  }
  if (torrent_parse(t, tbuf, (uint32_t)n) != 0) {
    puts("torrent: malformed .torrent\n");
    return -1;
  }
  return 0;
}

int main(int argc, char **argv)
{
  if (argc < 2) {
    puts("usage: torrent info|peers|peer|get <file> ... | torrent test\n");
    return 1;
  }
  const char *cmd = argv[1];

  if (streq(cmd, "test"))
    return torrent_selftest() == 0 ? 0 : 1;

  if (streq(cmd, "info")) {
    if (argc < 3) {
      puts("usage: torrent info <file>\n");
      return 1;
    }
    struct torrent t;
    if (load(argv[2], &t) != 0)
      return 1;
    torrent_print(&t);
    torrent_free(&t);
    return 0;
  }

  if (streq(cmd, "peers") || streq(cmd, "peer") || streq(cmd, "get")) {
    puts("torrent: ");
    puts(cmd);
    puts(" not yet available (tracker/peer port in progress)\n");
    return 1;
  }

  puts("torrent: unknown subcommand: ");
  puts(cmd);
  puts("\n");
  return 1;
}
