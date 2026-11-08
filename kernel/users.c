/* users.c — per-process identity resolution + login (see users.h). */

#include <stddef.h>

#include "crypto.h"
#include "fs.h"
#include "heap.h"
#include "kstring.h"
#include "sched.h"
#include "timer.h"
#include "users.h"

/* ---- tiny local string helpers (kstring.h is intentionally minimal) --------
 */

static void cpy(char *d, const char *s, int max)
{
  int i = 0;
  for (; s[i] && i < max - 1; i++)
    d[i] = s[i];
  d[i] = '\0';
}

/* Parse a non-negative decimal prefix of `s` into *out; return the char after
 * the digits. */
static const char *parse_u32(const char *s, uint32_t *out)
{
  uint32_t v = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (uint32_t)(*s - '0');
    s++;
  }
  *out = v;
  return s;
}

/* Lowercase-hex SHA-256 of `pass` into out[65] (64 hex + NUL). */
static void hash_pw(const char *pass, char out[65])
{
  uint8_t dg[SHA256_DIGEST];
  uint32_t n = 0;
  while (pass[n])
    n++;
  sha256(pass, n, dg);
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < SHA256_DIGEST; i++) {
    out[i * 2] = hx[(dg[i] >> 4) & 0xF];
    out[i * 2 + 1] = hx[dg[i] & 0xF];
  }
  out[64] = '\0';
}

/* Copy one ':'-delimited field starting at *p into buf[max]; advance *p past
 * the field (leaving it on the ':' or the line terminator). */
static void take_field(const char **p, char *buf, int max)
{
  const char *s = *p;
  int i = 0;
  while (*s && *s != ':' && *s != '\n' && *s != '\r') {
    if (i < max - 1)
      buf[i++] = *s;
    s++;
  }
  buf[i] = '\0';
  *p = s;
}

/* Advance `line` to the start of the next /etc/passwd line. */
static const char *next_line(const char *p)
{
  while (*p && *p != '\n')
    p++;
  while (*p == '\n' || *p == '\r')
    p++;
  return p;
}

/* One parsed passwd record. */
struct pwent {
  char name[USER_NAME_MAX];
  char pwhash[80];
  uint32_t uid, gid;
  char home[USER_HOME_MAX];
};

/* Parse the passwd line at `line` into *e. Returns the start of the next line.
 */
static const char *parse_line(const char *line, struct pwent *e)
{
  const char *p = line;
  e->pwhash[0] = e->home[0] = '\0';
  e->uid = e->gid = 0;
  take_field(&p, e->name, sizeof e->name);
  if (*p == ':') {
    p++;
    take_field(&p, e->pwhash, sizeof e->pwhash);
  }
  if (*p == ':') {
    p++;
    p = parse_u32(p, &e->uid);
  }
  if (*p == ':') {
    p++;
    p = parse_u32(p, &e->gid);
  }
  if (*p == ':') {
    p++;
    take_field(&p, e->home, sizeof e->home);
  }
  return next_line(p);
}

/* Look up the passwd record matching either a name (`want_name`, when non-NULL)
 * or a uid (`want_uid`). Returns 1 and fills *out on a hit, 0 if absent. */
static int passwd_lookup(const char *want_name, uint32_t want_uid, struct pwent *out)
{
  static char buf[4096];
  int n = fs_read("/etc/passwd", buf, sizeof buf - 1);
  if (n < 0)
    return 0;
  buf[n] = '\0';
  for (const char *line = buf; *line;) {
    struct pwent e;
    line = parse_line(line, &e);
    if (!e.name[0])
      continue;
    int hit = want_name ? streq(e.name, want_name) : (e.uid == want_uid);
    if (hit) {
      *out = e;
      return 1;
    }
  }
  return 0;
}

void users_init(void)
{
  /* The boot session is already uid 0 (sched.c initialises the main task to
   * root). The account database is created later in kernel_main; nothing else
   * to bring up here. */
}

void users_name_of(uint32_t uid, char *buf, int max)
{
  struct pwent e;
  if (passwd_lookup(NULL, uid, &e))
    cpy(buf, e.name, max);
  else
    cpy(buf, uid == 0 ? "root" : "user", max);
}

void users_home_of(uint32_t uid, char *buf, int max)
{
  struct pwent e;
  if (passwd_lookup(NULL, uid, &e) && e.home[0])
    cpy(buf, e.home, max);
  else
    cpy(buf, uid == 0 ? "/home/root" : "/home/user", max);
}

int users_login(const char *name, const char *pass)
{
  struct pwent e;
  if (!passwd_lookup(name, 0, &e))
    return -1; /* unknown user                       */

  if (e.pwhash[0]) { /* password required              */
    char given[65];
    hash_pw(pass ? pass : "", given);
    if (!streq(given, e.pwhash))
      return -2; /* wrong password                     */
  }

  sched_set_creds_self(e.uid, e.gid); /* switch the caller's identity   */
  return 0;
}

/* Validate `name`/`pass` against /etc/passwd WITHOUT changing any credentials,
 * and report the account's uid/gid/home. For the SSH server, which
 * authenticates a remote login from a kernel thread (so it must not touch its
 * own identity) and then spawns the user's shell with these creds. Returns 0 on
 * success, -1 unknown user, -2 wrong password. `home` may be NULL. */
int users_authenticate(const char *name, const char *pass, uint32_t *uid, uint32_t *gid, char *home, int homesz)
{
  struct pwent e;
  if (!passwd_lookup(name, 0, &e))
    return -1;
  if (e.pwhash[0]) {
    char given[65];
    hash_pw(pass ? pass : "", given);
    if (!streq(given, e.pwhash))
      return -2;
  }
  if (uid)
    *uid = e.uid;
  if (gid)
    *gid = e.gid;
  if (home)
    cpy(home, e.home[0] ? e.home : (e.uid == 0 ? "/home/root" : "/home/user"), homesz);
  return 0;
}

/* ---- sudo ---------------------------------------------------------------- */

/* Is `name` (or the account's primary group `gid`) in the group `group` of /etc/group? Lines are name:gid:members. */
static bool in_group(const char *group, const char *name, uint32_t gid)
{
  static char buf[4096];
  int n = fs_read("/etc/group", buf, sizeof buf - 1);
  if (n < 0)
    return false;
  buf[n] = '\0';
  uint32_t nlen = (uint32_t)strlen(name);
  for (const char *line = buf; *line; line = next_line(line)) {
    char gname[USER_NAME_MAX];
    const char *p = line;
    take_field(&p, gname, sizeof gname);
    if (!streq(gname, group) || *p != ':')
      continue;
    uint32_t ggid;
    p = parse_u32(p + 1, &ggid);
    if (ggid == gid)
      return true;
    if (*p != ':')
      return false;
    for (p++; *p && *p != '\n' && *p != '\r';) { /* comma separated members */
      const char *end = p;
      while (*end && *end != ',' && *end != '\n' && *end != '\r')
        end++;
      if ((uint32_t)(end - p) == nlen && memcmp(p, name, nlen) == 0)
        return true;
      p = *end == ',' ? end + 1 : end;
    }
    return false;
  }
  return false;
}

int users_sudo_allowed(uint32_t uid)
{
  struct pwent e;
  if (uid == 0)
    return 1;
  if (!passwd_lookup(NULL, uid, &e))
    return 0;
  return in_group("wheel", e.name, e.gid) || in_group("sudo", e.name, e.gid);
}

struct sudo_grant {
  uint32_t uid, expires; /* expires in timer ticks */
};
static struct sudo_grant *grants;
static uint32_t ngrants;

static struct sudo_grant *grant_of(uint32_t uid)
{
  for (uint32_t i = 0; i < ngrants; i++)
    if (grants[i].uid == uid)
      return &grants[i];
  return 0;
}

int users_sudo_grant(uint32_t uid, const char *pass)
{
  struct pwent e;
  if (!users_sudo_allowed(uid) || !passwd_lookup(NULL, uid, &e))
    return -1;
  if (users_authenticate(e.name, pass, 0, 0, 0, 0) != 0)
    return -2;
  struct sudo_grant *g = grant_of(uid);
  if (!g) {
    struct sudo_grant *grown = kmalloc((ngrants + 1) * sizeof *grown);
    if (!grown)
      return -1;
    memcpy(grown, grants, ngrants * sizeof *grown);
    kfree(grants);
    grants = grown;
    g = &grants[ngrants++];
    g->uid = uid;
  }
  g->expires = timer_ticks() + USERS_SUDO_SECONDS * TIMER_HZ;
  return 0;
}

int users_sudo_valid(uint32_t uid)
{
  if (uid == 0)
    return 1;
  struct sudo_grant *g = grant_of(uid);
  return g && (int32_t)(g->expires - timer_ticks()) > 0;
}

void users_sudo_revoke(uint32_t uid)
{
  struct sudo_grant *g = grant_of(uid);
  if (g)
    g->expires = timer_ticks();
}
