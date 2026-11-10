/* upnpc.c - manage port mappings on the UPnP Internet Gateway Device (the router) */

#include "opt.h"
#include "owos.h"
#include "upnp.h"

static int fail(const char *what, int rc)
{
  if (rc == UPNP_EINTR)
    return EXIT_SIGINT;
  if (rc == UPNP_EFAULT)
    printf("upnpc: %s refused by the gateway (UPnP error %d)\n", what, upnp_last_fault());
  else
    printf("upnpc: %s failed\n", what);
  return 1;
}

static int port_arg(const char *s, int *out)
{
  unsigned v;
  if (opt_uint(s, &v) != 0 || v < 1 || v > 65535)
    return 0;
  *out = (int)v;
  return 1;
}

int main(int argc, char **argv)
{
  static const struct opt opts[] = {{'h', "help", OPT_FLAG, 'h', 0, "show this help"},
                                    {'l', "lease", OPT_ARG, 'l', "SECS", "mapping lifetime, 0 = permanent (default 0)"},
                                    {'i', "ip", OPT_ARG, 'i', "ADDR", "internal client address (default: this host)"}};
  static const char *const synopsis = "[status | list | add TCP|UDP EXT [INT [DESC]] | del TCP|UDP EXT]";
  struct optstate st;
  const char *pos[6];
  int npos = 0;
  unsigned lease = 0;
  char ip[16] = "";

  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (o == 'h') {
      opt_print_help("upnpc", synopsis, "Discover the UPnP gateway and manage its port mappings.", opts, OPT_N(opts));
      return 0;
    }
    if (o == 'l' && opt_uint(st.arg, &lease) == 0)
      continue;
    if (o == 'i' && strlen(st.arg) < sizeof ip) {
      strcpy(ip, st.arg);
      continue;
    }
    if (o == OPT_POS && npos < 6) {
      pos[npos++] = st.arg;
      continue;
    }
    printf("upnpc: bad argument %s\n", o == OPT_POS ? st.arg : st.bad);
    opt_print_usage("upnpc", synopsis, opts, OPT_N(opts));
    return 1;
  }

  const char *cmd = npos ? pos[0] : "status";
  int ext = 0, in = 0;
  int is_add = streq(cmd, "add"), is_del = streq(cmd, "del");
  if (is_add || is_del) {
    if (npos < 3 || !port_arg(pos[2], &ext) || (!streq(pos[1], "TCP") && !streq(pos[1], "UDP")) ||
        (is_del && npos > 3) || (is_add && npos > 5) || (is_add && npos > 3 && !port_arg(pos[3], &in))) {
      opt_print_usage("upnpc", synopsis, opts, OPT_N(opts));
      return 1;
    }
    if (is_add && npos <= 3)
      in = ext;
  } else if (!streq(cmd, "status") && !streq(cmd, "list")) {
    printf("upnpc: unknown command %s\n", cmd);
    opt_print_usage("upnpc", synopsis, opts, OPT_N(opts));
    return 1;
  }

  struct upnp_igd g;
  int rc = upnp_discover(&g);
  if (rc == UPNP_EINTR)
    return EXIT_SIGINT;
  if (rc != 1) {
    printf("upnpc: no UPnP gateway found\n");
    return 1;
  }

  if (is_add) {
    if (!ip[0] && !upnp_local_ip(ip)) {
      printf("upnpc: no local address, pass --ip\n");
      return 1;
    }
    rc = upnp_add_mapping(&g, pos[1], ext, in, ip, npos > 4 ? pos[4] : "OwOS", lease);
    if (rc)
      return fail("add", rc);
    printf("%s %d -> %s:%d\n", pos[1], ext, ip, in);
    return 0;
  }
  if (is_del) {
    rc = upnp_del_mapping(&g, pos[1], ext);
    return rc ? fail("delete", rc) : 0;
  }

  if (streq(cmd, "status")) {
    char ext_ip[40];
    printf("gateway:   %s\n", g.ctl);
    printf("service:   %s\n", g.svc);
    rc = upnp_external_ip(&g, ext_ip, sizeof ext_ip);
    if (rc)
      return fail("external address query", rc);
    printf("external:  %s\n", ext_ip);
    return 0;
  }

  for (int i = 0;; i++) {
    struct upnp_map m;
    rc = upnp_get_mapping(&g, i, &m);
    if (rc == UPNP_EFAULT && upnp_last_fault() == 713)
      break;
    if (rc)
      return fail("list", rc);
    printf("%-3s %5d -> %s:%d%s  %s", m.proto, m.ext_port, m.int_ip, m.int_port, m.enabled ? "" : " (disabled)",
           m.desc);
    if (m.lease)
      printf("  (%us)", m.lease);
    printf("\n");
  }
  return 0;
}
