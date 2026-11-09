/* accounts.c: users, groups and logging in */

#include "sh.h"

/* multi-user: account & group management */

static char pbuf[8192]; /* working copy of /etc/passwd */

static char gbuf[8192]; /* working copy of /etc/group */

/* Load a line file into buf (NUL-terminated) */
static int load_file(const char *path, char *buf, int max)
{
  int n = readfile(path, buf, max - 1);
  if (n < 0) {
    buf[0] = '\0';
    return -1;
  }
  buf[n] = '\0';
  return n;
}

static int save_file(const char *path, const char *buf) { return writefile(path, buf, strlen(buf)); }

/* /etc/passwd into pbuf; if the caller may not read it, only their own line (from /proc/user) */
static void load_passwd(void)
{
  if (load_file("/etc/passwd", pbuf, sizeof pbuf) >= 0)
    return;
  char name[32], hm[80];
  int uid = getuid(), gid = getgid();
  if (!user_ident(name, sizeof name, hm, sizeof hm) || gid < 0) {
    pbuf[0] = '\0';
    return;
  }
  snprintf(pbuf, sizeof pbuf, "%s::%d:%d:%s\n", name, uid, gid, hm);
}

/* Unsigned int -> decimal string */
static void u2s(unsigned v, char *out)
{
  char t[12];
  int n = 0;
  if (!v) {
    out[0] = '0';
    out[1] = '\0';
    return;
  }
  while (v) {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  int i = 0;
  while (n)
    out[i++] = t[--n];
  out[i] = '\0';
}

/* Copy the idx-th ':'-delimited field of the line at `line` into out[max] */
static int line_field(const char *line, int idx, char *out, int max)
{
  const char *p = line;
  for (int f = 0; f < idx; f++) {
    while (*p && *p != ':' && *p != '\n')
      p++;
    if (*p != ':') {
      out[0] = '\0';
      return 0;
    }
    p++;
  }
  int j = 0;
  while (*p && *p != ':' && *p != '\n' && *p != '\r' && j < max - 1)
    out[j++] = *p++;
  out[j] = '\0';
  return 1;
}

/* Pointer to the line in `buf` whose field 0 equals `name`, or NULL */
static char *find_line(char *buf, const char *name)
{
  for (char *line = buf; *line;) {
    char nm[64];
    line_field(line, 0, nm, sizeof nm);
    if (strcmp(nm, name) == 0)
      return line;
    while (*line && *line != '\n')
      line++;
    if (*line == '\n')
      line++;
  }
  return 0;
}

/* Delete the line beginning at `p` from `buf` (shift the tail down over it) */
static void remove_line(char *buf, char *p)
{
  (void)buf;
  char *e = p;
  while (*e && *e != '\n')
    e++;
  if (*e == '\n')
    e++;
  int tail = strlen(e) + 1; /* include the NUL */
  for (int i = 0; i < tail; i++)
    p[i] = e[i];
}

/* Smallest id >= start in column `idx` that no line uses */
static unsigned next_free_id(const char *buf, int idx, unsigned start)
{
  for (unsigned c = start;; c++) {
    int taken = 0;
    for (const char *line = buf; *line;) {
      char f[16];
      line_field(line, idx, f, sizeof f);
      if (to_uint(f) == c) {
        taken = 1;
        break;
      }
      while (*line && *line != '\n')
        line++;
      if (*line == '\n')
        line++;
    }
    if (!taken)
      return c;
  }
}

/* Lowercase-hex SHA-256 of `pass` into out[65] */
static void pw_hash(const char *pass, char out[65])
{
  unsigned char dg[SHA256_DIGEST];
  sha256(pass, (unsigned)strlen(pass), dg);
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < SHA256_DIGEST; i++) {
    out[i * 2] = hx[(dg[i] >> 4) & 0xF];
    out[i * 2 + 1] = hx[dg[i] & 0xF];
  }
  out[64] = '\0';
}

static int require_root(const char *cmd)
{
  if (getuid() == 0)
    return 1;
  err(cmd, ": permission denied (must be root)");
  return 0;
}

/* A legal account/group name: [A-Za-z_][A-Za-z0-9_-]*, <= 16 chars */
static bool valid_name(const char *s)
{
  if (!s || !s[0])
    return false;
  int n = 0;
  for (const char *p = s; *p; p++, n++) {
    char c = *p;
    bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    bool rest = alpha || (c >= '0' && c <= '9') || c == '-';
    if (n == 0 ? !alpha : !rest)
      return false;
  }
  return n <= 16;
}

/* Does comma-separated list `csv` contain the exact token `tok`? */
static int csv_has(const char *csv, const char *tok)
{
  const char *p = csv;
  while (*p) {
    const char *s = p;
    while (*p && *p != ',')
      p++;
    int len = (int)(p - s), tl = strlen(tok);
    if (len == tl) {
      int eq = 1;
      for (int i = 0; i < len; i++)
        if (s[i] != tok[i]) {
          eq = 0;
          break;
        }
      if (eq)
        return 1;
    }
    if (*p == ',')
      p++;
  }
  return 0;
}

/* Group name whose gid field == gid, into out[max] ("" if none) */
static void group_name_for_gid(char *groups, unsigned gid, char *out, int max)
{
  out[0] = '\0';
  for (char *line = groups; *line;) {
    char gn[64], gf[16];
    line_field(line, 0, gn, sizeof gn);
    line_field(line, 1, gf, sizeof gf);
    if (to_uint(gf) == gid) {
      int i = 0;
      for (; gn[i] && i < max - 1; i++)
        out[i] = gn[i];
      out[i] = '\0';
      return;
    }
    while (*line && *line != '\n')
      line++;
    if (*line == '\n')
      line++;
  }
}

/* account commands (each is its own builtin) */

void cmd_useradd(int argc, char **argv)
{
  if (argc < 2) {
    err("usage: useradd <name>", 0);
    return;
  }
  if (!require_root("useradd"))
    return;
  const char *name = argv[1];
  if (!valid_name(name)) {
    err("useradd: invalid name: ", name);
    return;
  }

  load_file("/etc/passwd", pbuf, sizeof pbuf);
  if (find_line(pbuf, name)) {
    err("useradd: user already exists: ", name);
    return;
  }

  unsigned uid = next_free_id(pbuf, 2, 1000);
  unsigned gid = uid; /* user-private group (same id) */
  char ub[12], gb[12];
  u2s(uid, ub);
  u2s(gid, gb);

  char line[256];
  strcpy(line, name);
  strcat(line, "::"); /* empty pwhash = no password */
  strcat(line, ub);
  strcat(line, ":");
  strcat(line, gb);
  strcat(line, ":/home/");
  strcat(line, name);
  strcat(line, "\n");
  strcat(pbuf, line);
  if (save_file("/etc/passwd", pbuf) < 0) {
    err("useradd: cannot write /etc/passwd", 0);
    return;
  }

  /* The matching primary group */
  load_file("/etc/group", gbuf, sizeof gbuf);
  if (!find_line(gbuf, name)) {
    char gl[128];
    strcpy(gl, name);
    strcat(gl, ":");
    strcat(gl, gb);
    strcat(gl, ":\n");
    strcat(gbuf, gl);
    save_file("/etc/group", gbuf);
  }

  /* The home directory */
  char hp[64];
  strcpy(hp, "/home/");
  strcat(hp, name);
  if (mkdir(hp) == 0) {
    chown(hp, (int)uid, (int)gid);
    chmod(hp, 0700);
  }

  tputs("useradd: created ");
  tputs(name);
  tputs(" (uid=");
  tputs(ub);
  tputs(", home=");
  tputs(hp);
  tputs(")\n");
}

void cmd_userdel(int argc, char **argv)
{
  if (argc < 2) {
    err("usage: userdel <name>", 0);
    return;
  }
  if (!require_root("userdel"))
    return;
  const char *name = argv[1];
  if (strcmp(name, "root") == 0) {
    err("userdel: cannot remove root", 0);
    return;
  }
  if (strcmp(name, username) == 0) {
    err("userdel: cannot remove the current user", 0);
    return;
  }

  load_file("/etc/passwd", pbuf, sizeof pbuf);
  char *p = find_line(pbuf, name);
  if (!p) {
    err("userdel: no such user: ", name);
    return;
  }
  remove_line(pbuf, p);
  if (save_file("/etc/passwd", pbuf) < 0) {
    err("userdel: cannot write /etc/passwd", 0);
    return;
  }

  /* Drop the user's primary group and strip it from every member list */
  load_file("/etc/group", gbuf, sizeof gbuf);
  char *gp = find_line(gbuf, name);
  if (gp)
    remove_line(gbuf, gp);
  /* Rebuild each group line with `name` filtered out of its members */
  static char nb[8192];
  int o = 0;
  for (char *line = gbuf; *line;) {
    char gn[64], gf[16], mem[512];
    line_field(line, 0, gn, sizeof gn);
    line_field(line, 1, gf, sizeof gf);
    line_field(line, 2, mem, sizeof mem);
    /* name:gid: */
    for (int i = 0; gn[i]; i++)
      nb[o++] = gn[i];
    nb[o++] = ':';
    for (int i = 0; gf[i]; i++)
      nb[o++] = gf[i];
    nb[o++] = ':';
    /* filtered members */
    bool first = true;
    for (const char *q = mem; *q;) {
      const char *s = q;
      while (*q && *q != ',')
        q++;
      int len = (int)(q - s);
      char tok[64];
      int tl = len < 63 ? len : 63;
      for (int i = 0; i < tl; i++)
        tok[i] = s[i];
      tok[tl] = '\0';
      if (tok[0] && strcmp(tok, name) != 0) {
        if (!first)
          nb[o++] = ',';
        for (int i = 0; i < tl; i++)
          nb[o++] = tok[i];
        first = false;
      }
      if (*q == ',')
        q++;
    }
    nb[o++] = '\n';
    while (*line && *line != '\n')
      line++;
    if (*line == '\n')
      line++;
  }
  nb[o] = '\0';
  save_file("/etc/group", nb);

  tputs("userdel: removed ");
  tputs(name);
  tputs(" (home /home/");
  tputs(name);
  tputs(" left in place)\n");
}

void cmd_passwd(int argc, char **argv)
{
  const char *target, *newpass;
  int clear = 0;
  if (argc >= 3 && strcmp(argv[1], "-d") == 0) {
    target = argv[2];
    newpass = "";
    clear = 1;
  } else if (argc >= 3) {
    target = argv[1];
    newpass = argv[2];
  } else if (argc == 2) {
    target = username;
    newpass = argv[1];
  } else {
    err("usage: passwd [user] <newpassword>   (passwd -d <user> clears it)", 0);
    return;
  }

  if (getuid() != 0) {
    err("passwd: permission denied (/etc/passwd is root-only)", 0);
    return;
  }

  load_file("/etc/passwd", pbuf, sizeof pbuf);
  char *p = find_line(pbuf, target);
  if (!p) {
    err("passwd: no such user: ", target);
    return;
  }

  char f0[32], f2[16], f3[16], f4[80];
  line_field(p, 0, f0, sizeof f0);
  line_field(p, 2, f2, sizeof f2);
  line_field(p, 3, f3, sizeof f3);
  line_field(p, 4, f4, sizeof f4);
  char hash[65];
  if (clear)
    hash[0] = '\0';
  else
    pw_hash(newpass, hash);

  remove_line(pbuf, p);
  char nl[256];
  strcpy(nl, f0);
  strcat(nl, ":");
  strcat(nl, hash);
  strcat(nl, ":");
  strcat(nl, f2);
  strcat(nl, ":");
  strcat(nl, f3);
  strcat(nl, ":");
  strcat(nl, f4);
  strcat(nl, "\n");
  strcat(pbuf, nl);
  if (save_file("/etc/passwd", pbuf) < 0) {
    err("passwd: cannot write /etc/passwd", 0);
    return;
  }

  tputs(clear ? "passwd: password cleared for " : "passwd: password updated for ");
  tputs(target);
  tputc('\n');
}

void cmd_groupadd(int argc, char **argv)
{
  if (argc < 2) {
    err("usage: groupadd <name>", 0);
    return;
  }
  if (!require_root("groupadd"))
    return;
  const char *name = argv[1];
  if (!valid_name(name)) {
    err("groupadd: invalid name: ", name);
    return;
  }

  load_file("/etc/group", gbuf, sizeof gbuf);
  if (find_line(gbuf, name)) {
    err("groupadd: group already exists: ", name);
    return;
  }

  unsigned gid = next_free_id(gbuf, 1, 100);
  char gb[12];
  u2s(gid, gb);
  char gl[128];
  strcpy(gl, name);
  strcat(gl, ":");
  strcat(gl, gb);
  strcat(gl, ":\n");
  strcat(gbuf, gl);
  if (save_file("/etc/group", gbuf) < 0) {
    err("groupadd: cannot write /etc/group", 0);
    return;
  }

  tputs("groupadd: created group ");
  tputs(name);
  tputs(" (gid=");
  tputs(gb);
  tputs(")\n");
}

void cmd_groupdel(int argc, char **argv)
{
  if (argc < 2) {
    err("usage: groupdel <name>", 0);
    return;
  }
  if (!require_root("groupdel"))
    return;
  const char *name = argv[1];
  if (strcmp(name, "root") == 0) {
    err("groupdel: cannot remove the root group", 0);
    return;
  }

  load_file("/etc/group", gbuf, sizeof gbuf);
  char *gp = find_line(gbuf, name);
  if (!gp) {
    err("groupdel: no such group: ", name);
    return;
  }
  remove_line(gbuf, gp);
  if (save_file("/etc/group", gbuf) < 0) {
    err("groupdel: cannot write /etc/group", 0);
    return;
  }

  tputs("groupdel: removed group ");
  tputs(name);
  tputc('\n');
}

/* usermod -aG <group> <user>: add an existing user to a supplementary group */
void cmd_usermod(int argc, char **argv)
{
  if (argc < 4 || strcmp(argv[1], "-aG") != 0) {
    err("usage: usermod -aG <group> <user>", 0);
    return;
  }
  if (!require_root("usermod"))
    return;
  const char *grp = argv[2], *usr = argv[3];

  load_file("/etc/passwd", pbuf, sizeof pbuf);
  if (!find_line(pbuf, usr)) {
    err("usermod: no such user: ", usr);
    return;
  }

  load_file("/etc/group", gbuf, sizeof gbuf);
  char *gp = find_line(gbuf, grp);
  if (!gp) {
    err("usermod: no such group: ", grp);
    return;
  }

  char f0[64], f1[16], f2[512];
  line_field(gp, 0, f0, sizeof f0);
  line_field(gp, 1, f1, sizeof f1);
  line_field(gp, 2, f2, sizeof f2);
  if (csv_has(f2, usr)) {
    tputs(usr);
    tputs(" is already in group ");
    tputs(grp);
    tputc('\n');
    return;
  }

  remove_line(gbuf, gp);
  char nl[640];
  strcpy(nl, f0);
  strcat(nl, ":");
  strcat(nl, f1);
  strcat(nl, ":");
  if (f2[0]) {
    strcat(nl, f2);
    strcat(nl, ",");
  }
  strcat(nl, usr);
  strcat(nl, "\n");
  strcat(gbuf, nl);
  if (save_file("/etc/group", gbuf) < 0) {
    err("usermod: cannot write /etc/group", 0);
    return;
  }

  tputs("usermod: added ");
  tputs(usr);
  tputs(" to group ");
  tputs(grp);
  tputc('\n');
}

/* users: list every account */
void cmd_users(int, char **)
{
  load_passwd();
  for (char *line = pbuf; *line;) {
    char nm[32], uid[16], hm[80];
    line_field(line, 0, nm, sizeof nm);
    line_field(line, 2, uid, sizeof uid);
    line_field(line, 4, hm, sizeof hm);
    if (nm[0]) {
      tputs_c(nm, FG_DIR);
      tputs("  uid=");
      tputs(uid);
      tputs("  ");
      tputs(hm);
      tputc('\n');
    }
    while (*line && *line != '\n')
      line++;
    if (*line == '\n')
      line++;
  }
}

/* The groups a user belongs to: its primary group plus any whose member list names it */
static void user_groups(const char *user, char *out, int max)
{
  out[0] = '\0';
  int o = 0;
  load_passwd();
  char *p = find_line(pbuf, user);
  unsigned pgid = 0;
  if (p) {
    char g[16];
    line_field(p, 3, g, sizeof g);
    pgid = to_uint(g);
  }

  load_file("/etc/group", gbuf, sizeof gbuf);
  /* primary group first */
  char primary[64];
  group_name_for_gid(gbuf, pgid, primary, sizeof primary);
  if (primary[0])
    for (int i = 0; primary[i] && o < max - 1; i++)
      out[o++] = primary[i];

  for (char *line = gbuf; *line;) {
    char gn[64], gf[16], mem[512];
    line_field(line, 0, gn, sizeof gn);
    line_field(line, 1, gf, sizeof gf);
    line_field(line, 2, mem, sizeof mem);
    if (csv_has(mem, user) && strcmp(gn, primary) != 0) {
      if (o > 0 && o < max - 1)
        out[o++] = ' ';
      for (int i = 0; gn[i] && o < max - 1; i++)
        out[o++] = gn[i];
    }
    while (*line && *line != '\n')
      line++;
    if (*line == '\n')
      line++;
  }
  out[o] = '\0';
}

void cmd_groups(int argc, char **argv)
{
  const char *u = argc >= 2 ? argv[1] : username;
  load_passwd();
  if (!find_line(pbuf, u)) {
    err("groups: no such user: ", u);
    return;
  }
  char gl[256];
  user_groups(u, gl, sizeof gl);
  tputs(u);
  tputs(" : ");
  tputs(gl);
  tputc('\n');
}

void cmd_id(int argc, char **argv)
{
  const char *u = argc >= 2 ? argv[1] : username;
  load_passwd();
  char *p = find_line(pbuf, u);
  if (!p) {
    err("id: no such user: ", u);
    return;
  }
  char uid[16], gid[16];
  line_field(p, 2, uid, sizeof uid);
  line_field(p, 3, gid, sizeof gid);

  load_file("/etc/group", gbuf, sizeof gbuf);
  char pgrp[64];
  group_name_for_gid(gbuf, to_uint(gid), pgrp, sizeof pgrp);
  char gl[256];
  user_groups(u, gl, sizeof gl);

  tputs("uid=");
  tputs(uid);
  tputs("(");
  tputs(u);
  tputs(") ");
  tputs("gid=");
  tputs(gid);
  tputs("(");
  tputs(pgrp[0] ? pgrp : u);
  tputs(") ");
  tputs("groups=");
  tputs(gl);
  tputc('\n');
}

/* su / login / logout: switch this shell's identity */
void do_login(const char *name, const char *pass)
{
  char req[200];
  strcpy(req, name);
  if (pass && pass[0]) {
    strcat(req, " ");
    strcat(req, pass);
  }
  if (writefile("/proc/login", req, strlen(req)) < 0) {
    err("su: authentication failure", 0);
    return;
  }
  reload_identity();
  strcpy(cwd, home);
  chdir(cwd);
  sync_env();
  tputs("now logged in as ");
  tputs_c(username, FG_PROMPT);
  tputc('\n');
}

void cmd_su(int argc, char **argv)
{
  const char *name = argc >= 2 ? argv[1] : "root";
  const char *pass = argc >= 3 ? argv[2] : 0;
  do_login(name, pass);
}

void cmd_login(int argc, char **argv)
{
  if (argc < 2) {
    err("usage: login <user> [password]", 0);
    return;
  }
  do_login(argv[1], argc >= 3 ? argv[2] : 0);
}

void bi_whoami(int, char **)
{
  tputs(username);
  tputc('\n');
}

/* hostname: print the system hostname, or (with an arg) set it via /etc/hostname */
void bi_hostname(int argc, char **argv)
{
  if (argc > 1) {
    if (sethostname(argv[1]) < 0) {
      err("hostname: cannot write /etc/hostname", 0);
      return;
    }
    gethostname(hostnm, sizeof hostnm);
  } else {
    tputs(hostnm);
    tputc('\n');
  }
}
