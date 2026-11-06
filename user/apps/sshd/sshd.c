/* sshd.c — the SSH-2 server, entirely in user space.
 *
 * The server analogue of user/apps/ssh/ssh.c: curve25519-sha256 KEX +
 * aes128-gcm@openssh.com over a raw TCP listener (net_listen/net_accept), the
 * exchange hash signed with an ssh-ed25519 host key. Password auth against
 * /etc/passwd (name:sha256(pw):uid:gid:home). On success it spawns /bin/term as
 * the authenticated user via spawn_cap_io_as (the root-only spawn-as-user
 * primitive) and relays the channel to the shell's stdio. One client at a time.
 *
 * The daemon is started as root at boot; each session drops to the logged-in
 * user's uid/gid — the Unix model, ported out of the old kernel/sshd.c.
 */

#include "crypto.h"
#include "ed25519.h"
#include "owos.h"

/* ---- SSH-2 message numbers ----------------------------------------------- */
#define MSG_DISCONNECT 1
#define MSG_IGNORE 2
#define MSG_UNIMPLEMENTED 3
#define MSG_DEBUG 4
#define MSG_SERVICE_REQUEST 5
#define MSG_SERVICE_ACCEPT 6
#define MSG_EXT_INFO 7
#define MSG_KEXINIT 20
#define MSG_NEWKEYS 21
#define MSG_KEX_ECDH_INIT 30
#define MSG_KEX_ECDH_REPLY 31
#define MSG_USERAUTH_REQUEST 50
#define MSG_USERAUTH_FAILURE 51
#define MSG_USERAUTH_SUCCESS 52
#define MSG_USERAUTH_BANNER 53
#define MSG_GLOBAL_REQUEST 80
#define MSG_REQUEST_SUCCESS 81
#define MSG_REQUEST_FAILURE 82
#define MSG_CHANNEL_OPEN 90
#define MSG_CHANNEL_OPEN_CONFIRMATION 91
#define MSG_CHANNEL_OPEN_FAILURE 92
#define MSG_CHANNEL_WINDOW_ADJUST 93
#define MSG_CHANNEL_DATA 94
#define MSG_CHANNEL_EXTENDED_DATA 95
#define MSG_CHANNEL_EOF 96
#define MSG_CHANNEL_CLOSE 97
#define MSG_CHANNEL_REQUEST 98
#define MSG_CHANNEL_SUCCESS 99
#define MSG_CHANNEL_FAILURE 100

#define SERVER_IDENT "SSH-2.0-OwOS_sshd"
#define MAXPKT 35000
#define HOSTKEY_PATH "/etc/ssh_host_ed25519"

/* ---- per-connection state (single static instance) ----------------------- */

struct ssh {
  int sock;
  int dead;

  unsigned char rx[40960];
  int rxlen;

  unsigned char payload[MAXPKT];
  unsigned char ptext[MAXPKT];   /* recv GCM plaintext / caller build scratch */
  unsigned char sealbuf[MAXPKT]; /* send GCM plaintext (distinct from ptext)  */
  unsigned char tx[MAXPKT + 64];

  int send_enc, recv_enc;
  unsigned char send_key[16], recv_key[16];
  unsigned char send_iv[12], recv_iv[12];

  unsigned char H[32], session_id[32];
  unsigned char kmp[40];
  int kmp_len;

  char client_ident[256];
  unsigned char ic[8192];
  int ic_len; /* client KEXINIT (I_C) */
  unsigned char is[2048];
  int is_len; /* our KEXINIT    (I_S) */

  /* channel */
  unsigned peer_chan, peer_window, peer_maxpkt;
  unsigned local_window;

  char user[64];
  int uid, gid;
  char home[192];
};

static struct ssh S;

/* The persistent host key. */
static uint8_t host_seed[32], host_pub[32];
static int host_ready;

/* ---- byte helpers -------------------------------------------------------- */

static void wb(unsigned char *d, int *p, unsigned v) { d[(*p)++] = (unsigned char)v; }
static void w32(unsigned char *d, int *p, unsigned v)
{
  d[(*p)++] = (unsigned char)(v >> 24);
  d[(*p)++] = (unsigned char)(v >> 16);
  d[(*p)++] = (unsigned char)(v >> 8);
  d[(*p)++] = (unsigned char)v;
}
static void wbytes(unsigned char *d, int *p, const void *s, int n)
{
  memcpy(d + *p, s, (unsigned)n);
  *p += n;
}
static void wstr(unsigned char *d, int *p, const void *s, int n)
{
  w32(d, p, (unsigned)n);
  wbytes(d, p, s, n);
}
static void wcstr(unsigned char *d, int *p, const char *s) { wstr(d, p, s, (int)strlen(s)); }
static unsigned r32(const unsigned char *d, int *p)
{
  unsigned v = ((unsigned)d[*p] << 24) | ((unsigned)d[*p + 1] << 16) | ((unsigned)d[*p + 2] << 8) | (unsigned)d[*p + 3];
  *p += 4;
  return v;
}

static void incr_iv(unsigned char *iv)
{
  for (int i = 11; i >= 4; i--)
    if (++iv[i] != 0)
      break;
}

/* ---- transport ----------------------------------------------------------- */

static int sock_send(const unsigned char *b, int n)
{
  int off = 0;
  while (off < n) {
    int w = net_send(S.sock, b + off, n - off);
    if (w <= 0) {
      S.dead = 1;
      return -1;
    }
    off += w;
  }
  return 0;
}

/* Pull whatever is available off the socket (non-blocking).
 * Returns bytes added, 0 would-block, -1 on EOF/error/EINTR. */
static int rx_pump(void)
{
  if (S.rxlen >= (int)sizeof S.rx)
    return 0;
  int n = net_recv_nb(S.sock, S.rx + S.rxlen, (int)sizeof S.rx - S.rxlen);
  if (n > 0) {
    S.rxlen += n;
    return n;
  }
  if (n == 0)
    return 0;
  S.dead = 1;
  return -1; /* -1 EOF/closed, NET_EINTR on Ctrl+C */
}

static void rx_consume(int n)
{
  if (n >= S.rxlen) {
    S.rxlen = 0;
    return;
  }
  memmove(S.rx, S.rx + n, (unsigned)(S.rxlen - n));
  S.rxlen -= n;
}

static int send_packet(const unsigned char *pl, int plen)
{
  int blk = S.send_enc ? 16 : 8;
  int lenfield = S.send_enc ? 0 : 4; /* RFC4253 counts the length field; RFC5647 GCM doesn't */
  int padlen = blk - ((lenfield + 1 + plen) % blk);
  if (padlen < 4)
    padlen += blk;
  unsigned pktlen = (unsigned)(1 + plen + padlen);
  int pos = 0;

  if (!S.send_enc) {
    w32(S.tx, &pos, pktlen);
    wb(S.tx, &pos, (unsigned)padlen);
    wbytes(S.tx, &pos, pl, plen);
    rng_bytes(S.tx + pos, padlen);
    pos += padlen;
    return sock_send(S.tx, pos);
  }
  unsigned char aad[4];
  int ap = 0;
  w32(aad, &ap, pktlen);
  int pp = 0;
  wb(S.sealbuf, &pp, (unsigned)padlen);
  wbytes(S.sealbuf, &pp, pl, plen);
  rng_bytes(S.sealbuf + pp, padlen);
  pp += padlen;
  w32(S.tx, &pos, pktlen);
  unsigned char tag[16];
  aes128_gcm_seal(S.send_key, S.send_iv, aad, 4, S.sealbuf, (unsigned)pp, S.tx + pos, tag);
  pos += pp;
  memcpy(S.tx + pos, tag, 16);
  pos += 16;
  incr_iv(S.send_iv);
  return sock_send(S.tx, pos);
}

/* 1 = decoded one packet into payload (+*plen), consumed; 0 = need more; -1 err
 */
static int parse_one(int *plen)
{
  if (S.rxlen < 4)
    return 0;
  unsigned pktlen =
      ((unsigned)S.rx[0] << 24) | ((unsigned)S.rx[1] << 16) | ((unsigned)S.rx[2] << 8) | (unsigned)S.rx[3];
  if (pktlen < 8 || pktlen > MAXPKT)
    return -1;

  if (!S.recv_enc) {
    if (4 + (int)pktlen > S.rxlen)
      return 0;
    int padlen = S.rx[4];
    int payl = (int)pktlen - padlen - 1;
    if (payl < 0 || payl > MAXPKT)
      return -1;
    memcpy(S.payload, S.rx + 5, (unsigned)payl);
    *plen = payl;
    rx_consume(4 + (int)pktlen);
    return 1;
  }
  if (4 + (int)pktlen + 16 > S.rxlen)
    return 0;
  unsigned char aad[4] = {S.rx[0], S.rx[1], S.rx[2], S.rx[3]};
  if (aes128_gcm_open(S.recv_key, S.recv_iv, aad, 4, S.rx + 4, pktlen, S.ptext, S.rx + 4 + pktlen) != 0)
    return -1;
  incr_iv(S.recv_iv);
  int padlen = S.ptext[0];
  int payl = (int)pktlen - padlen - 1;
  if (payl < 0 || payl > MAXPKT)
    return -1;
  memcpy(S.payload, S.ptext + 1, (unsigned)payl);
  *plen = payl;
  rx_consume(4 + (int)pktlen + 16);
  return 1;
}

static int read_packet(int *plen, int timeout_ms)
{
  unsigned start = msec();
  for (;;) {
    int r = parse_one(plen);
    if (r == 1)
      return 0;
    if (r < 0)
      return -1;
    int p = rx_pump();
    if (p < 0)
      return -1;
    if (p == 0) {
      if (timeout_ms >= 0 && (int)(msec() - start) > timeout_ms)
        return -1;
      msleep(2);
    } else {
      start = msec();
    }
  }
}

/* Next non-transport message; auto-handle IGNORE/DEBUG/EXT_INFO/GLOBAL_REQUEST.
 */
static int recv_msg(int *plen, int timeout_ms)
{
  for (;;) {
    if (read_packet(plen, timeout_ms) != 0)
      return -1;
    int t = S.payload[0];
    if (t == MSG_IGNORE || t == MSG_DEBUG || t == MSG_UNIMPLEMENTED || t == MSG_EXT_INFO)
      continue;
    if (t == MSG_DISCONNECT)
      return -1;
    if (t == MSG_GLOBAL_REQUEST) {
      int p = 1;
      unsigned n = r32(S.payload, &p);
      int wr = (p + (int)n < *plen) ? S.payload[p + (int)n] : 0;
      if (wr) {
        unsigned char f = MSG_REQUEST_FAILURE;
        send_packet(&f, 1);
      }
      continue;
    }
    return t;
  }
}

/* ---- host key ------------------------------------------------------------ */

static void host_key_init(void)
{
  if (host_ready)
    return;
  int n = readfile(HOSTKEY_PATH, (char *)host_seed, 32);
  if (n != 32) {
    rng_bytes(host_seed, 32);
    writefile(HOSTKEY_PATH, host_seed, 32); /* persist a fresh host key */
  }
  ed25519_pubkey(host_pub, host_seed);
  host_ready = 1;
}

/* ---- KEX ----------------------------------------------------------------- */

static void build_kexinit(void)
{
  int p = 0;
  wb(S.is, &p, MSG_KEXINIT);
  unsigned char cookie[16];
  rng_bytes(cookie, 16);
  wbytes(S.is, &p, cookie, 16);
  wcstr(S.is, &p, "curve25519-sha256,curve25519-sha256@libssh.org");
  wcstr(S.is, &p, "ssh-ed25519");
  wcstr(S.is, &p, "aes128-gcm@openssh.com"); /* enc c->s */
  wcstr(S.is, &p, "aes128-gcm@openssh.com"); /* enc s->c */
  wcstr(S.is, &p, "hmac-sha2-256");          /* mac c->s (AEAD: unused) */
  wcstr(S.is, &p, "hmac-sha2-256");          /* mac s->c (AEAD: unused) */
  wcstr(S.is, &p, "none");
  wcstr(S.is, &p, "none");
  wcstr(S.is, &p, "");
  wcstr(S.is, &p, "");
  wb(S.is, &p, 0);
  w32(S.is, &p, 0);
  S.is_len = p;
}

static void derive(unsigned char letter, unsigned char *out, int outlen)
{
  struct sha256 h;
  unsigned char block[32];
  sha256_init(&h);
  sha256_update(&h, S.kmp, (unsigned)S.kmp_len);
  sha256_update(&h, S.H, 32);
  sha256_update(&h, &letter, 1);
  sha256_update(&h, S.session_id, 32);
  sha256_final(&h, block);
  int got = 0;
  for (;;) {
    int c = (outlen - got) < 32 ? (outlen - got) : 32;
    memcpy(out + got, block, (unsigned)c);
    got += c;
    if (got >= outlen)
      break;
    sha256_init(&h);
    sha256_update(&h, S.kmp, (unsigned)S.kmp_len);
    sha256_update(&h, S.H, 32);
    sha256_update(&h, out, (unsigned)got);
    sha256_final(&h, block);
  }
}

static int span_has(const unsigned char *hay, int n, const char *needle)
{
  int m = (int)strlen(needle);
  for (int i = 0; i + m <= n; i++) {
    int k = 0;
    while (k < m && hay[i + k] == (unsigned char)needle[k])
      k++;
    if (k == m)
      return 1;
  }
  return 0;
}

static int do_kex(void)
{
  int plen;

  build_kexinit();
  if (send_packet(S.is, S.is_len) != 0)
    return -1;

  if (recv_msg(&plen, 15000) != MSG_KEXINIT)
    return -1;
  if (plen > (int)sizeof S.ic)
    return -1;
  memcpy(S.ic, S.payload, (unsigned)plen);
  S.ic_len = plen;
  if (!span_has(S.ic, S.ic_len, "curve25519-sha256"))
    return -1;
  if (!span_has(S.ic, S.ic_len, "aes128-gcm@openssh.com"))
    return -1;

  /* client's ephemeral Q_C */
  if (recv_msg(&plen, 15000) != MSG_KEX_ECDH_INIT)
    return -1;
  int p = 1;
  unsigned qclen = r32(S.payload, &p);
  if (qclen != 32 || p + 32 > plen)
    return -1;
  unsigned char qc[32];
  memcpy(qc, S.payload + p, 32);

  /* our ephemeral d_s / Q_S, shared K */
  unsigned char priv[32], qs[32], shared[32];
  rng_bytes(priv, 32);
  priv[0] &= 248;
  priv[31] &= 127;
  priv[31] |= 64;
  static const unsigned char base[32] = {9};
  x25519(qs, priv, base);
  x25519(shared, priv, qc);

  /* mpint(K) */
  {
    int i = 0;
    while (i < 32 && shared[i] == 0)
      i++;
    int nn = 32 - i, z = (nn > 0 && (shared[i] & 0x80)) ? 1 : 0, kp = 0;
    w32(S.kmp, &kp, (unsigned)(nn + z));
    if (z)
      S.kmp[kp++] = 0;
    memcpy(S.kmp + kp, shared + i, (unsigned)nn);
    kp += nn;
    S.kmp_len = kp;
  }

  /* K_S = string("ssh-ed25519") || string(host_pub) */
  unsigned char ks[64];
  int ksl = 0;
  wcstr(ks, &ksl, "ssh-ed25519");
  wstr(ks, &ksl, host_pub, 32);

  /* H = SHA256(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || mpint(K)) */
  {
    struct sha256 h;
    unsigned char lp[4];
    int z;
    sha256_init(&h);
    z = 0;
    w32(lp, &z, (unsigned)strlen(S.client_ident));
    sha256_update(&h, lp, 4);
    sha256_update(&h, S.client_ident, (unsigned)strlen(S.client_ident));
    z = 0;
    w32(lp, &z, (unsigned)strlen(SERVER_IDENT));
    sha256_update(&h, lp, 4);
    sha256_update(&h, SERVER_IDENT, (unsigned)strlen(SERVER_IDENT));
    z = 0;
    w32(lp, &z, (unsigned)S.ic_len);
    sha256_update(&h, lp, 4);
    sha256_update(&h, S.ic, (unsigned)S.ic_len);
    z = 0;
    w32(lp, &z, (unsigned)S.is_len);
    sha256_update(&h, lp, 4);
    sha256_update(&h, S.is, (unsigned)S.is_len);
    z = 0;
    w32(lp, &z, (unsigned)ksl);
    sha256_update(&h, lp, 4);
    sha256_update(&h, ks, (unsigned)ksl); /* K_S as a string */
    z = 0;
    w32(lp, &z, 32);
    sha256_update(&h, lp, 4);
    sha256_update(&h, qc, 32);
    z = 0;
    w32(lp, &z, 32);
    sha256_update(&h, lp, 4);
    sha256_update(&h, qs, 32);
    sha256_update(&h, S.kmp, (unsigned)S.kmp_len);
    sha256_final(&h, S.H);
  }
  memcpy(S.session_id, S.H, 32);

  /* sign H with the host key: sig blob = string("ssh-ed25519") || string(sig)
   */
  unsigned char sig[64];
  ed25519_sign(sig, S.H, 32, host_seed, host_pub);
  unsigned char sb[128];
  int sbl = 0;
  wcstr(sb, &sbl, "ssh-ed25519");
  wstr(sb, &sbl, sig, 64);

  /* KEX_ECDH_REPLY: string K_S, string Q_S, string sig */
  p = 0;
  wb(S.ptext, &p, MSG_KEX_ECDH_REPLY);
  wstr(S.ptext, &p, ks, ksl);
  wstr(S.ptext, &p, qs, 32);
  wstr(S.ptext, &p, sb, sbl);
  if (send_packet(S.ptext, p) != 0)
    return -1;

  /* keys: server receives C2S (A/C), sends S2C (B/D) */
  derive('A', S.recv_iv, 12);
  derive('C', S.recv_key, 16);
  derive('B', S.send_iv, 12);
  derive('D', S.send_key, 16);

  unsigned char nk = MSG_NEWKEYS;
  if (send_packet(&nk, 1) != 0)
    return -1;
  S.send_enc = 1;
  if (recv_msg(&plen, 15000) != MSG_NEWKEYS)
    return -1;
  S.recv_enc = 1;
  return 0;
}

/* ---- auth ---------------------------------------------------------------- */

/* Extract an SSH string field at *p into out (NUL-terminated, capped). */
static void get_str(int *p, int plen, char *out, int outsz)
{
  unsigned n = (*p + 4 <= plen) ? r32(S.payload, p) : 0;
  int i = 0;
  for (; i < (int)n && *p < plen && i < outsz - 1; i++)
    out[i] = (char)S.payload[(*p)++];
  out[i] = '\0';
  *p += (int)n - i; /* skip any remainder we truncated */
}

/* Field `idx` of a colon-delimited /etc/passwd line [s,end). */
static void pw_field(const char *s, const char *end, int idx, char *out, int max)
{
  int f = 0;
  while (f < idx && s < end) {
    if (*s == ':')
      f++;
    s++;
  }
  int i = 0;
  while (s < end && *s != ':' && i < max - 1)
    out[i++] = *s++;
  out[i] = '\0';
}

static void hex_sha256(const char *s, char out[65])
{
  uint8_t dg[SHA256_DIGEST];
  sha256(s, (unsigned)strlen(s), dg);
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < SHA256_DIGEST; i++) {
    out[i * 2] = hx[(dg[i] >> 4) & 0xF];
    out[i * 2 + 1] = hx[dg[i] & 0xF];
  }
  out[64] = '\0';
}

/* Password auth against /etc/passwd (name:sha256hex(pw):uid:gid:home). An empty
 * password field accepts any password, matching the kernel's
 * users_authenticate. On success fills S.uid/gid/home. Returns 0 ok, -1 on no
 * match / bad password. */
static int passwd_check(const char *user, const char *pass)
{
  static char pw[4096];
  int n = readfile("/etc/passwd", pw, (int)sizeof pw - 1);
  if (n <= 0)
    return -1;
  pw[n] = '\0';
  char want[65];
  hex_sha256(pass ? pass : "", want);
  for (const char *p = pw; *p;) {
    const char *eol = p;
    while (*eol && *eol != '\n')
      eol++;
    char nm[64];
    pw_field(p, eol, 0, nm, sizeof nm);
    if (streq(nm, user)) {
      char hash[80], uf[12], gf[12], home[192];
      pw_field(p, eol, 1, hash, sizeof hash);
      pw_field(p, eol, 2, uf, sizeof uf);
      pw_field(p, eol, 3, gf, sizeof gf);
      pw_field(p, eol, 4, home, sizeof home);
      if (hash[0] && !streq(hash, want))
        return -1;
      int uid = 0, gid = 0;
      for (const char *c = uf; *c >= '0' && *c <= '9'; c++)
        uid = uid * 10 + (*c - '0');
      for (const char *c = gf; *c >= '0' && *c <= '9'; c++)
        gid = gid * 10 + (*c - '0');
      S.uid = uid;
      S.gid = gid;
      if (home[0])
        strcpy(S.home, home); /* home <= 191 (pw_field cap) */
      else
        strcpy(S.home, uid == 0 ? "/home/root" : "/home/user");
      return 0;
    }
    p = (*eol == '\n') ? eol + 1 : eol;
  }
  return -1;
}

static int do_auth(void)
{
  int plen;
  if (recv_msg(&plen, 15000) != MSG_SERVICE_REQUEST)
    return -1;
  {
    unsigned char a[1 + 4 + 12];
    int p = 0;
    wb(a, &p, MSG_SERVICE_ACCEPT);
    wcstr(a, &p, "ssh-userauth");
    if (send_packet(a, p) != 0)
      return -1;
  }

  for (int tries = 0; tries < 6; tries++) {
    int t = recv_msg(&plen, 60000);
    if (t < 0)
      return -1;
    if (t != MSG_USERAUTH_REQUEST)
      continue;
    int p = 1;
    char user[64], svc[32], method[32];
    get_str(&p, plen, user, sizeof user);
    get_str(&p, plen, svc, sizeof svc);
    get_str(&p, plen, method, sizeof method);

    if (streq(method, "password")) {
      p += 1; /* boolean FALSE (no change req) */
      char pass[128];
      get_str(&p, plen, pass, sizeof pass);
      if (passwd_check(user, pass) == 0) {
        unsigned char s = MSG_USERAUTH_SUCCESS;
        if (send_packet(&s, 1) != 0)
          return -1;
        strcpy(S.user, user); /* user <= 63 (get_str cap) */
        return 0;
      }
    }
    /* "none" or a failed password: report password as the only method. */
    unsigned char f[1 + 4 + 8 + 1];
    int q = 0;
    wb(f, &q, MSG_USERAUTH_FAILURE);
    wcstr(f, &q, "password");
    wb(f, &q, 0); /* partial success = FALSE */
    if (send_packet(f, q) != 0)
      return -1;
  }
  return -1;
}

/* ---- channel + session --------------------------------------------------- */

#define LOCAL_WINDOW 0x100000u
#define LOCAL_MAXPKT 32768u

static int send_data(const unsigned char *d, int n)
{
  int off = 0;
  while (off < n) {
    int chunk = n - off;
    if ((unsigned)chunk > S.peer_maxpkt)
      chunk = (int)S.peer_maxpkt;
    if ((unsigned)chunk > S.peer_window)
      chunk = (int)S.peer_window;
    if (chunk <= 0)
      return off;
    int p = 0;
    wb(S.ptext, &p, MSG_CHANNEL_DATA);
    w32(S.ptext, &p, S.peer_chan);
    wstr(S.ptext, &p, d + off, chunk);
    if (send_packet(S.ptext, p) != 0)
      return -1;
    S.peer_window -= (unsigned)chunk;
    off += chunk;
  }
  return off;
}

static int top_up_window(void)
{
  if (S.local_window >= LOCAL_WINDOW / 2)
    return 0;
  unsigned add = LOCAL_WINDOW - S.local_window;
  int p = 0;
  wb(S.ptext, &p, MSG_CHANNEL_WINDOW_ADJUST);
  w32(S.ptext, &p, S.peer_chan);
  w32(S.ptext, &p, add);
  if (send_packet(S.ptext, p) != 0)
    return -1;
  S.local_window += add;
  return 0;
}

/* Run the login shell: spawn /bin/term in headless "-stdio" mode AS the authed
 * user (spawn_cap_io_as: the root-only spawn-as-user primitive, cwd = home),
 * with its stdin/stdout wired to pipes we drive, then relay bytes both ways.
 * The shell doesn't know it's SSH — the Unix model. Returns when the client
 * closes the channel or the shell exits. */
static void ssh_session(void)
{
  const char *fail = "no shell available\r\n";
  char *argv[2] = {"term", "-stdio"};
  int rfd = -1, wfd = -1;
  int pid = spawn_cap_io_as("/bin/term", argv, 2, S.uid, S.gid, S.home[0] ? S.home : "/", &rfd, &wfd);
  if (pid < 0 || rfd < 0 || wfd < 0) {
    if (pid >= 0) {
      kill(pid, SIGKILL);
      waitpid(pid);
    }
    if (rfd >= 0)
      close(rfd);
    if (wfd >= 0)
      close(wfd);
    send_data((const unsigned char *)fail, (int)strlen(fail));
    return;
  }

  unsigned char buf[1024];
  int done = 0, stdin_closed = 0;
  while (!done) {
    int progressed = 0;

    int rn = read_nb(rfd, buf, (int)sizeof buf);
    if (rn > 0) {
      if (send_data(buf, rn) < 0)
        break;
      progressed = 1;
    } else if (rn < 0)
      break; /* shell exited: its stdout end closed */

    if (rx_pump() < 0)
      break;
    int plen, r;
    while ((r = parse_one(&plen)) == 1) {
      progressed = 1;
      int t = S.payload[0];
      if (t == MSG_CHANNEL_DATA) {
        int p = 1;
        r32(S.payload, &p);
        unsigned dn = r32(S.payload, &p);
        if (dn <= (unsigned)(plen - p) && !stdin_closed) {
          write(wfd, S.payload + p, (int)dn);
          S.local_window = S.local_window > dn ? S.local_window - dn : 0;
        }
      } else if (t == MSG_CHANNEL_WINDOW_ADJUST) {
        int p = 1;
        r32(S.payload, &p);
        S.peer_window += r32(S.payload, &p);
      } else if (t == MSG_CHANNEL_EOF) {
        /* client won't send more stdin: close the shell's stdin so it sees
         * EOF, but keep relaying its stdout until the shell itself exits. */
        if (!stdin_closed) {
          close(wfd);
          stdin_closed = 1;
        }
      } else if (t == MSG_CHANNEL_CLOSE || t == MSG_DISCONNECT) {
        done = 1;
        break;
      }
    }
    if (r < 0)
      break;
    if (top_up_window() < 0)
      break;
    if (S.dead)
      break;
    if (!progressed)
      msleep(4);
  }

  if (!stdin_closed)
    close(wfd);       /* drop our stdin writer -> shell sees EOF */
  close(rfd);         /* drop our stdout reader                  */
  kill(pid, SIGKILL); /* stop the shell if still running         */
  waitpid(pid);       /* reap it                                 */
}

static int do_channel(void)
{
  int plen;
  if (recv_msg(&plen, 15000) != MSG_CHANNEL_OPEN)
    return -1;
  int p = 1;
  char kind[32];
  get_str(&p, plen, kind, sizeof kind);
  unsigned peer = r32(S.payload, &p);
  unsigned pwin = r32(S.payload, &p);
  unsigned pmax = r32(S.payload, &p);
  if (!streq(kind, "session")) {
    unsigned char f[64];
    int q = 0;
    wb(f, &q, MSG_CHANNEL_OPEN_FAILURE);
    w32(f, &q, peer);
    w32(f, &q, 3);
    wcstr(f, &q, "only session");
    wcstr(f, &q, "");
    send_packet(f, q);
    return -1;
  }
  S.peer_chan = peer;
  S.peer_window = pwin;
  S.peer_maxpkt = (pmax && pmax <= MAXPKT) ? pmax : 32768;
  S.local_window = LOCAL_WINDOW;

  unsigned char a[64];
  int q = 0;
  wb(a, &q, MSG_CHANNEL_OPEN_CONFIRMATION);
  w32(a, &q, peer); /* recipient = client's channel */
  w32(a, &q, 0);    /* our channel id */
  w32(a, &q, LOCAL_WINDOW);
  w32(a, &q, LOCAL_MAXPKT);
  if (send_packet(a, q) != 0)
    return -1;

  /* Handle channel requests until shell/exec starts the session. */
  for (;;) {
    int t = recv_msg(&plen, 60000);
    if (t < 0)
      return -1;
    if (t != MSG_CHANNEL_REQUEST) {
      if (t == MSG_CHANNEL_CLOSE)
        return -1;
      continue;
    }
    int rp = 1;
    r32(S.payload, &rp); /* recipient channel */
    char req[32];
    get_str(&rp, plen, req, sizeof req);
    int want_reply = (rp < plen) ? S.payload[rp] : 0;

    if (streq(req, "pty-req") || streq(req, "env")) {
      if (want_reply) {
        unsigned char r[5];
        int z = 0;
        wb(r, &z, MSG_CHANNEL_SUCCESS);
        w32(r, &z, S.peer_chan);
        send_packet(r, z);
      }
      continue;
    }
    if (streq(req, "shell") || streq(req, "exec") || streq(req, "subsystem")) {
      if (want_reply) {
        unsigned char r[5];
        int z = 0;
        wb(r, &z, MSG_CHANNEL_SUCCESS);
        w32(r, &z, S.peer_chan);
        send_packet(r, z);
      }
      ssh_session();
      return 0;
    }
    if (want_reply) {
      unsigned char r[5];
      int z = 0;
      wb(r, &z, MSG_CHANNEL_FAILURE);
      w32(r, &z, S.peer_chan);
      send_packet(r, z);
    }
  }
}

static void channel_close(void)
{
  unsigned char m[5];
  int p;
  p = 0;
  wb(m, &p, MSG_CHANNEL_EOF);
  w32(m, &p, S.peer_chan);
  send_packet(m, p);
  p = 0;
  wb(m, &p, MSG_CHANNEL_CLOSE);
  w32(m, &p, S.peer_chan);
  send_packet(m, p);
}

/* ---- version exchange + top level ---------------------------------------- */

static int read_ident(void)
{
  unsigned start = msec();
  for (;;) {
    int nl = -1;
    for (int i = 0; i < S.rxlen; i++)
      if (S.rx[i] == '\n') {
        nl = i;
        break;
      }
    if (nl >= 0) {
      int len = nl;
      if (len > 0 && S.rx[len - 1] == '\r')
        len--;
      if (len >= 4 && S.rx[0] == 'S' && S.rx[1] == 'S' && S.rx[2] == 'H' && S.rx[3] == '-') {
        int c = len < (int)sizeof S.client_ident - 1 ? len : (int)sizeof S.client_ident - 1;
        memcpy(S.client_ident, S.rx, (unsigned)c);
        S.client_ident[c] = '\0';
        rx_consume(nl + 1);
        return 0;
      }
      rx_consume(nl + 1);
      continue;
    }
    int p = rx_pump();
    if (p < 0)
      return -1;
    if (p == 0) {
      if ((int)(msec() - start) > 15000)
        return -1;
      msleep(3);
    } else
      start = msec();
  }
}

/* Serve one SSH connection synchronously over the accepted socket `c`. */
static void serve(int c)
{
  memset(&S, 0, sizeof S);
  S.sock = c;
  host_key_init();

  if (sock_send((const unsigned char *)SERVER_IDENT "\r\n", (int)strlen(SERVER_IDENT) + 2) != 0)
    return;
  if (read_ident() != 0)
    return;
  if (do_kex() != 0)
    return;
  if (do_auth() != 0)
    return;
  do_channel();
  channel_close();
}

int main(void)
{
  int lfd = net_listen(22);
  if (lfd < 0) {
    puts("sshd: cannot listen on port 22\n");
    return 1;
  }
  for (;;) {
    int c = net_accept(lfd);
    if (c < 0) {
      if (c == NET_EINTR)
        break;
      msleep(50);
      continue;
    }
    serve(c);
    net_close(c);
  }
  net_close(lfd);
  return 0;
}
