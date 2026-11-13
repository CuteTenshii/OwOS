/* ps.c - report a snapshot of the running processes, read from /proc/ps.
 * ps [-eAfTL] [-u USER[,USER]] [-p PID[,PID]] [--no-headers]
 * With no selection it lists the caller's own processes. */
#include "opt.h"
#include "owos.h"
#include <stdbool.h>

#define MAX_TASKS 1024
#define MAX_SEL 64
#define USER_COL 8

struct task {
  unsigned tid, pid, ppid, uid, runs;
  char state;
  char name[16];
  char tname[32];
};

static char procbuf[32768]; /* the kernel's PSEUDO_FILE_MAX */
static struct task tasks[MAX_TASKS];
static char pwbuf[8192];
static bool have_passwd;

static char outbuf[4096];
static int outlen;

static void out_flush(void)
{
  if (outlen > 0)
    write(1, outbuf, outlen);
  outlen = 0;
}

static void out_str(const char *s)
{
  for (; *s; s++) {
    if (outlen == (int)sizeof outbuf)
      out_flush();
    outbuf[outlen++] = *s;
  }
}

/* Left-aligned in `width` columns, then one separating space. */
static void out_left(const char *s, int width)
{
  out_str(s);
  for (int pad = width - strlen(s); pad > 0; pad--)
    out_str(" ");
  out_str(" ");
}

/* Right-aligned in `width` columns, then one separating space. */
static void out_num(unsigned v, int width)
{
  char t[16];
  snprintf(t, sizeof t, "%u", v);
  for (int pad = width - strlen(t); pad > 0; pad--)
    out_str(" ");
  out_str(t);
  out_str(" ");
}

/* Copy the idx-th `sep`-delimited field of [s, end) into out; false if the line has fewer fields. */
static bool field(const char *s, const char *end, char sep, int idx, char *out, int cap)
{
  for (int f = 0; f < idx; f++) {
    while (s < end && *s != sep)
      s++;
    if (s >= end)
      return false;
    s++;
  }
  int n = 0;
  while (s < end && *s != sep && *s != '\r' && n < cap - 1)
    out[n++] = *s++;
  out[n] = '\0';
  return true;
}

static bool field_uint(const char *s, const char *end, char sep, int idx, unsigned *v)
{
  char t[12];
  return field(s, end, sep, idx, t, sizeof t) && opt_uint(t, v) == 0;
}

static int load_tasks(void)
{
  int len = readfile("/proc/ps", procbuf, (int)sizeof procbuf);
  if (len < 0)
    return -1;
  int n = 0;
  for (const char *p = procbuf, *end = procbuf + len; p < end && n < MAX_TASKS;) {
    const char *eol = p;
    while (eol < end && *eol != '\n')
      eol++;
    struct task *t = &tasks[n];
    char st[2];
    if (field_uint(p, eol, '\t', 0, &t->tid) && field_uint(p, eol, '\t', 1, &t->pid) &&
        field_uint(p, eol, '\t', 2, &t->ppid) && field_uint(p, eol, '\t', 3, &t->uid) &&
        field(p, eol, '\t', 4, st, sizeof st) && field_uint(p, eol, '\t', 5, &t->runs) &&
        field(p, eol, '\t', 6, t->name, sizeof t->name)) {
      if (!field(p, eol, '\t', 7, t->tname, sizeof t->tname))
        t->tname[0] = '\0'; /* kernels without thread names stop at the program name */
      t->state = st[0];
      n++;
    }
    p = eol + 1;
  }
  return n;
}

static void sort_tasks(int n)
{
  for (int i = 1; i < n; i++) {
    struct task key = tasks[i];
    int j = i - 1;
    while (j >= 0 && (tasks[j].pid > key.pid || (tasks[j].pid == key.pid && tasks[j].tid > key.tid))) {
      tasks[j + 1] = tasks[j];
      j--;
    }
    tasks[j + 1] = key;
  }
}

/* Unreadable for non-root callers on some systems; lookups then fall back to the caller's own identity. */
static void load_passwd(void)
{
  int n = readfile("/etc/passwd", pwbuf, (int)sizeof pwbuf - 1);
  have_passwd = n >= 0;
  pwbuf[have_passwd ? n : 0] = '\0';
}

static void uid_name(unsigned uid, char *out, int cap)
{
  for (const char *p = pwbuf; have_passwd && *p;) {
    const char *eol = p;
    while (*eol && *eol != '\n')
      eol++;
    unsigned u;
    if (field_uint(p, eol, ':', 2, &u) && u == uid && field(p, eol, ':', 0, out, cap) && out[0])
      return;
    p = *eol ? eol + 1 : eol;
  }
  if ((int)uid == getuid() && user_ident(out, cap, 0, 0))
    return;
  snprintf(out, (unsigned)cap, "%u", uid);
}

static bool name_uid(const char *name, unsigned *uid)
{
  if (opt_uint(name, uid) == 0)
    return true;
  char f[64];
  for (const char *p = pwbuf; have_passwd && *p;) {
    const char *eol = p;
    while (*eol && *eol != '\n')
      eol++;
    if (field(p, eol, ':', 0, f, sizeof f) && streq(f, name) && field_uint(p, eol, ':', 2, uid))
      return true;
    p = *eol ? eol + 1 : eol;
  }
  if (user_ident(f, sizeof f, 0, 0) && streq(f, name)) {
    *uid = (unsigned)getuid();
    return true;
  }
  return false;
}

struct sel {
  unsigned v[MAX_SEL];
  int n;
};

static bool sel_has(const struct sel *s, unsigned v)
{
  for (int i = 0; i < s->n; i++)
    if (s->v[i] == v)
      return true;
  return false;
}

/* Add each comma-separated item of `list` to `s`; prints the error and returns false on a bad item. */
static bool sel_add_list(struct sel *s, const char *list, bool users)
{
  const char *end = list + strlen(list);
  for (int idx = 0;; idx++) {
    char item[64];
    if (!field(list, end, ',', idx, item, sizeof item))
      return true;
    unsigned v;
    if (!item[0] || !(users ? name_uid(item, &v) : opt_uint(item, &v) == 0)) {
      printf("ps: %s '%s'\n", users ? "unknown user" : "invalid process id", item);
      return false;
    }
    if (s->n == MAX_SEL) {
      printf("ps: too many %s (at most %d)\n", users ? "users" : "process ids", MAX_SEL);
      return false;
    }
    s->v[s->n++] = v;
  }
}

enum { O_EVERY = 1, O_FULL, O_THREADS, O_USER, O_PID, O_NOHEAD, O_HELP };

static const struct opt opts[] = {
    {'e', "every", OPT_FLAG, O_EVERY, 0, "select every process"},
    {'A', 0, OPT_FLAG, O_EVERY, 0, "same as -e"},
    {'f', "full", OPT_FLAG, O_FULL, 0, "full format: user, parent pid, state and run count"},
    {'T', "threads", OPT_FLAG, O_THREADS, 0, "show each thread, with its thread id and name"},
    {'L', 0, OPT_FLAG, O_THREADS, 0, "same as -T"},
    {'u', "user", OPT_ARG, O_USER, "USER[,USER]", "select processes owned by these users (name or uid)"},
    {'p', "pid", OPT_ARG, O_PID, "PID[,PID]", "select these process ids"},
    {0, "no-headers", OPT_FLAG, O_NOHEAD, 0, "don't print the header line"},
    {'h', "help", OPT_FLAG, O_HELP, 0, "show this help"},
};

int main(int argc, char **argv)
{
  bool every = false, full = false, threads = false, headers = true;
  static struct sel pids, uids;

  load_passwd();
  struct optstate st;
  opt_init(&st, argc, argv);
  for (int c; (c = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (c == OPT_POS) {
      printf("ps: invalid operand '%s'\n", st.arg);
      opt_print_usage("ps", 0, opts, OPT_N(opts));
      return 1;
    }
    if (c == OPT_BAD) {
      printf("ps: invalid option '%s'\n", st.bad);
      opt_print_usage("ps", 0, opts, OPT_N(opts));
      return 1;
    }
    if (c == O_HELP) {
      opt_print_help("ps", 0, "Report a snapshot of the running processes (default: your own).", opts, OPT_N(opts));
      return 0;
    }
    if (c == O_EVERY)
      every = true;
    else if (c == O_FULL)
      full = true;
    else if (c == O_THREADS)
      threads = true;
    else if (c == O_NOHEAD)
      headers = false;
    else if (!sel_add_list(c == O_USER ? &uids : &pids, st.arg, c == O_USER))
      return 1;
  }

  int n = load_tasks();
  if (n < 0) {
    puts("ps: cannot read /proc/ps\n");
    return 1;
  }
  sort_tasks(n);
  unsigned self_uid = (unsigned)getuid();

  if (headers) {
    if (full)
      out_left("USER", USER_COL);
    out_str("  PID ");
    if (threads)
      out_str("  TID ");
    if (full)
      out_str(" PPID S     RUNS ");
    out_str("CMD\n");
  }

  for (int i = 0; i < n; i++) {
    const struct task *t = &tasks[i];
    if (!threads && t->tid != t->pid)
      continue;
    bool picked =
        every || sel_has(&pids, t->pid) || sel_has(&uids, t->uid) || (!pids.n && !uids.n && t->uid == self_uid);
    if (!picked)
      continue;
    if (full) {
      char user[64];
      uid_name(t->uid, user, sizeof user);
      out_left(user, USER_COL);
    }
    out_num(t->pid, 5);
    if (threads)
      out_num(t->tid, 5);
    if (full) {
      out_num(t->ppid, 5);
      char s[3] = {t->state, ' ', '\0'};
      out_str(s);
      out_num(t->runs, 8);
    }
    out_str(t->name);
    if (threads && t->tname[0]) {
      out_str(" [");
      out_str(t->tname);
      out_str("]");
    }
    out_str("\n");
  }
  out_flush();
  return 0;
}
