/* ntpd.c, the NTPv4 client daemon, in user space. Autostarted detached at boot.
 *
 * Polls a few servers, keeps the lowest-delay result, and steps or slews the wall clock
 * (SYS_SETTIME / SYS_ADJTIME). `ntpd -1` syncs once and exits. */

#include "ntp.h"
#include "opt.h"
#include "owos.h"
#include "resolve.h"
#include <stdbool.h>
#include <stdint.h>

#define CONF_PATH "/etc/ntp.conf"
#define SAMPLES_PER_SERVER 4
#define SAMPLE_GAP_MS 2000
#define REPLY_TIMEOUT_MS 2000
#define MAX_PEERS 3 /* servers consulted per round once they answer */

static const char *const default_servers[] = {
    "0.pool.ntp.org",      "1.pool.ntp.org",  "2.pool.ntp.org", "3.pool.ntp.org",
    "time.cloudflare.com", "time.google.com", "162.159.200.1",
};

enum { O_ONCE = 1, O_QUIET, O_SERVER, O_VERBOSE, O_HELP };

static const struct opt opts[] = {
    {'1', "once", OPT_FLAG, O_ONCE, 0, "sync once and exit"},
    {'q', "quiet", OPT_FLAG, O_QUIET, 0, "log nothing"},
    {'s', "server", OPT_ARG, O_SERVER, "HOST", "query HOST instead of the configured servers"},
    {'v', "verbose", OPT_FLAG, O_VERBOSE, 0, "log every exchange"},
    {'h', "help", OPT_FLAG, O_HELP, 0, "show this help"},
};

static bool verbose, quiet;

struct server_list {
  const char **names;
  int n;
};

struct result {
  struct ntp_sample s;
  const char *server;
};

static int64_t tv_to_us(const struct timeval64 *tv) { return tv->tv_sec * 1000000 + tv->tv_usec; }

static struct timeval64 us_to_tv(int64_t us)
{
  struct timeval64 tv = {us / 1000000, (int)(us % 1000000)};
  if (tv.tv_usec < 0) {
    tv.tv_usec += 1000000;
    tv.tv_sec--;
  }
  return tv;
}

static int64_t now_us(void)
{
  struct timeval64 tv;
  gettime(&tv);
  return tv_to_us(&tv);
}

/* "+12.345 ms" */
static void fmt_ms(char *out, unsigned size, int64_t us)
{
  int64_t a = us < 0 ? -us : us;
  snprintf(out, size, "%c%d.%03d ms", us < 0 ? '-' : '+', (int)(a / 1000), (int)(a % 1000));
}

static bool add_server(struct server_list *l, const char *name)
{
  const char **v = realloc(l->names, (unsigned)(l->n + 1) * sizeof *v);
  if (!v)
    return false;
  l->names = v;
  v[l->n++] = name;
  return true;
}

/* `server <host>` lines of /etc/ntp.conf; the buffer stays allocated for the name pointers. */
static void load_conf(struct server_list *l)
{
  struct statinfo si;
  if (stat(CONF_PATH, &si) != 0 || si.is_dir || si.size == 0)
    return;
  char *buf = malloc(si.size + 1);
  if (!buf)
    return;
  int len = readfile(CONF_PATH, buf, (int)si.size);
  if (len <= 0) {
    free(buf);
    return;
  }
  buf[len] = '\0';
  for (char *line = buf; *line;) {
    char *next = line;
    while (*next && *next != '\n')
      next++;
    if (*next)
      *next++ = '\0';
    while (*line == ' ' || *line == '\t')
      line++;
    if (memcmp(line, "server", 6) == 0 && (line[6] == ' ' || line[6] == '\t')) {
      char *host = line + 6;
      while (*host == ' ' || *host == '\t')
        host++;
      char *end = host;
      while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '#')
        end++;
      *end = '\0';
      if (*host)
        add_server(l, host);
    }
    line = next;
  }
}

/* One request/reply exchange. False on timeout or an unusable reply. */
static bool exchange(int h, struct ntp_sample *out)
{
  uint16_t rnd = 0;
  getrandom(&rnd, sizeof rnd);
  struct ntp_ts sent = ntp_ts_from_unix_us(now_us());
  sent.frac = (sent.frac & ~0xffffu) | rnd;
  int64_t t1 = ntp_ts_to_unix_us(sent);

  uint8_t pkt[NTP_PKT_LEN];
  ntp_build_request(pkt, sent);
  if (net_send(h, pkt, sizeof pkt) < 0)
    return false;

  unsigned start = msec();
  while ((int)(msec() - start) < REPLY_TIMEOUT_MS) {
    uint8_t buf[NTP_PKT_LEN + 64];
    int n = net_recv_nb(h, buf, (int)sizeof buf);
    if (n == 0) {
      msleep(2);
      continue;
    }
    int64_t t4 = now_us();
    struct ntp_packet p;
    enum ntp_status st = n < 0 ? NTP_BAD_LEN : ntp_parse(buf, n, &p);
    if (st == NTP_OK)
      st = ntp_validate(&p, sent);
    if (st == NTP_BAD_ORIGIN || st == NTP_BAD_MODE || st == NTP_BAD_LEN) {
      if (n < 0)
        return false;
      continue; /* stale or foreign datagram: keep waiting */
    }
    if (st != NTP_OK) {
      if (verbose && !quiet)
        printf("ntpd: rejected reply: %s\n", ntp_status_str(st));
      return false;
    }
    int64_t t2 = ntp_ts_to_unix_us(p.rx), t3 = ntp_ts_to_unix_us(p.tx);
    out->offset_us = ntp_offset_us(t1, t2, t3, t4);
    out->delay_us = ntp_delay_us(t1, t2, t3, t4);
    return true;
  }
  return false;
}

static bool query_server(const char *host, struct result *res)
{
  int h = net_dial(host, NTP_PORT, SK_UDP);
  if (h == NET_EINTR)
    exit(EXIT_SIGINT);
  if (h < 0) {
    if (verbose && !quiet)
      printf("ntpd: cannot reach %s\n", host);
    return false;
  }

  struct ntp_sample samples[SAMPLES_PER_SERVER];
  int n = 0;
  for (int i = 0; i < SAMPLES_PER_SERVER; i++) {
    if (i)
      msleep(SAMPLE_GAP_MS);
    if (exchange(h, &samples[n]))
      n++;
    else if (i == 0)
      break;
  }
  net_close(h);

  res->server = host;
  return ntp_filter(samples, n, &res->s);
}

static bool sync_round(const struct server_list *l, struct result *best)
{
  struct result peers[MAX_PEERS];
  int got = 0;
  for (int i = 0; i < l->n && got < MAX_PEERS; i++)
    if (query_server(l->names[i], &peers[got]))
      got++;
  if (!got)
    return false;
  int pick = 0;
  for (int i = 1; i < got; i++)
    if (peers[i].s.delay_us < peers[pick].s.delay_us)
      pick = i;
  *best = peers[pick];
  return true;
}

/* Stores the last sync under /var or /tmp when one exists. */
static void write_state(const struct result *r, int64_t at_us)
{
  static const char *const dirs[] = {"/var/run", "/var", "/tmp"};
  for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
    struct statinfo si;
    if (stat(dirs[i], &si) != 0 || !si.is_dir)
      continue;
    char path[64], body[256];
    snprintf(path, sizeof path, "%s/ntp.state", dirs[i]);
    int len = snprintf(body, sizeof body, "server=%s\noffset_us=%d\ndelay_us=%d\ntime=%u\n", r->server,
                       (int)r->s.offset_us, (int)r->s.delay_us, (unsigned)(at_us / 1000000));
    writefile(path, body, len);
    return;
  }
}

int main(int argc, char **argv)
{
  bool once = false;
  const char *forced = 0;
  struct optstate st;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (o == O_ONCE)
      once = true;
    else if (o == O_QUIET)
      quiet = true;
    else if (o == O_VERBOSE)
      verbose = true;
    else if (o == O_SERVER)
      forced = st.arg;
    else if (o == O_HELP) {
      opt_print_help("ntpd", 0, "Keep the clock in sync over NTP.", opts, OPT_N(opts));
      return 0;
    } else {
      opt_print_usage("ntpd", 0, opts, OPT_N(opts));
      return 2;
    }
  }

  struct server_list servers = {0, 0};
  if (forced) {
    add_server(&servers, forced);
  } else {
    load_conf(&servers);
    if (!servers.n)
      for (unsigned i = 0; i < sizeof default_servers / sizeof default_servers[0]; i++)
        add_server(&servers, default_servers[i]);
  }

  int poll = NTP_POLL_MIN, failures = 0;
  bool synced = false;
  for (;;) {
    struct result best;
    if (!sync_round(&servers, &best)) {
      poll = NTP_POLL_MIN;
      failures++;
      if (once) {
        if (!quiet)
          puts("ntpd: no usable reply from any server\n");
        return 1;
      }
      if (verbose && !quiet)
        printf("ntpd: no usable reply, retrying in %ds\n", ntp_retry_delay(failures));
      msleep((unsigned)ntp_retry_delay(failures) * 1000u);
      continue;
    }
    failures = 0;

    int64_t offset = best.s.offset_us;
    bool step = ntp_should_step(offset, !synced);
    bool ok;
    if (step) {
      struct timeval64 tv = us_to_tv(now_us() + offset);
      ok = settime(&tv) == 0;
    } else {
      struct timeval64 delta = us_to_tv(offset);
      ok = adjtime(&delta, 0) == 0;
    }
    if (!ok) {
      if (!quiet)
        puts("ntpd: cannot set the clock (need root)\n");
      return 1;
    }
    synced = true;
    poll = ntp_next_poll(poll, offset);
    write_state(&best, now_us());

    if (!quiet && (verbose || once)) {
      char off[24], dly[24];
      fmt_ms(off, sizeof off, offset);
      fmt_ms(dly, sizeof dly, best.s.delay_us);
      printf("ntpd: %s offset %s delay %s, %s", best.server, off, dly, step ? "stepped" : "slewed");
      if (once)
        puts("\n");
      else
        printf(", next poll %ds\n", poll);
    }
    if (once)
      return 0;
    msleep((unsigned)poll * 1000u);
  }
}
