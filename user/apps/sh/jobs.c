/* jobs.c: running programs: the foreground wait, background jobs and their builtins */

#include "sh.h"

/* Background jobs: programs running without the terminal's attention. A job has the shell's output but no input
 * (its stdin is closed), so it cannot steal keystrokes meant for the prompt. */
struct job {
  int id, pid;
  char *cmd;
};

static struct job *jobs;

static int njobs, next_job_id = 1;

int last_bg_pid; /* for $! */

static void job_add(int pid, const char *cmd)
{
  struct job *grown = realloc(jobs, (unsigned)(njobs + 1) * sizeof *jobs);
  char *copy = malloc(strlen(cmd) + 1);
  if (!grown || !copy) {
    free(copy);
    return;
  }
  strcpy(copy, cmd);
  jobs = grown;
  jobs[njobs++] = (struct job){next_job_id++, pid, copy};
  last_bg_pid = pid;
  tputc('[');
  tdec((unsigned)jobs[njobs - 1].id);
  tputs("] ");
  tdec((unsigned)pid);
  tputc('\n');
}

static void job_remove(int index)
{
  free(jobs[index].cmd);
  jobs[index] = jobs[--njobs];
  if (!njobs)
    next_job_id = 1;
}

/* "%n", "%+", "%%" or a bare number: a job index, or -1 */
static int job_find(const char *spec)
{
  if (spec[0] == '%')
    spec++;
  if (!spec[0] || streq(spec, "+") || streq(spec, "%"))
    return njobs - 1;
  int id = 0;
  for (const char *c = spec; *c; c++) {
    if (*c < '0' || *c > '9')
      return -1;
    id = id * 10 + (*c - '0');
  }
  for (int i = 0; i < njobs; i++)
    if (jobs[i].id == id)
      return i;
  return -1;
}

/* Report and forget the jobs that finished since the last prompt */
void bg_service(void)
{
  for (int i = 0; i < njobs;) {
    int status;
    if (!waitpid_nb(jobs[i].pid, &status)) {
      i++;
      continue;
    }
    tputc('[');
    tdec((unsigned)jobs[i].id);
    tputs(status ? "]  Exit " : "]  Done ");
    if (status)
      tdec((unsigned)status);
    tputs("  ");
    tputs(jobs[i].cmd);
    tputc('\n');
    job_remove(i);
  }
}

/* The command line being run, for naming jobs */
const char *current_line = "";

/* Wait for a foreground process. One that opens its own window is handed off and the prompt returns. */
static void wait_foreground(int pid)
{
  tty_set_fg(1, pid);
  for (;;) {
    if (waitpid_nb(pid, &last_status))
      break;
    if (win_owned(pid)) {
      job_add(pid, current_line);
      break;
    }
    msleep(4);
  }
  tty_set_fg(1, 0);
}

/* Run a /bin program on our terminal: wait for it, or with `background` return at once */
static void run_program(const char *path, char **argv, int argc, bool background)
{
  oflush();
  int quiet[2] = {-1, -1}; /* a background job reads end of file instead of the keyboard */
  int out[2] = {-1, -1};   /* the program's stdout, when the shell is redirecting it */
  struct spawn_opts o = {.path = path, .argv = argv, .argc = argc, .flags = SPAWN_STDIO, .stdio = {0, 1, 2}};
  if (background && pipe(quiet) == 0)
    o.stdio[0] = quiet[0];
  if (out_capturing() && pipe(out) == 0)
    o.stdio[1] = out[1];
  int pid = spawn_ex(&o);
  if (out[1] >= 0)
    close(out[1]);
  if (quiet[0] >= 0) {
    close(quiet[0]);
    close(quiet[1]);
  }
  if (pid < 0) {
    if (out[0] >= 0)
      close(out[0]);
    err(pid == -EACCES ? "permission denied: " : "failed to launch ", path);
    return;
  }
  if (out[0] >= 0) {
    char buf[512];
    int n;
    tty_set_fg(1, pid);
    while ((n = read(out[0], buf, sizeof buf)) > 0)
      out_capture_put(buf, (unsigned)n);
    close(out[0]);
  }
  if (background)
    job_add(pid, current_line);
  else
    wait_foreground(pid);
}

void run_other(int argc, char **argv, bool background)
{
  char path[256];
  if (find_command(argv[0], path, sizeof path))
    run_program(path, argv, argc, background);
  else
    err(argv[0], ": command not found");
}

void bi_jobs(int, char **)
{
  for (int i = 0; i < njobs; i++) {
    tputc('[');
    tdec((unsigned)jobs[i].id);
    tputs(i == njobs - 1 ? "]+  Running  " : "]-  Running  ");
    tputs(jobs[i].cmd);
    tputc('\n');
  }
}

/* fg brings a job under the terminal's control: Ctrl+C reaches it, and the shell waits for it */
void bi_fg(int argc, char **argv)
{
  int i = job_find(argc > 1 ? argv[1] : "");
  if (i < 0) {
    err("fg: no such job", 0);
    return;
  }
  tputs(jobs[i].cmd);
  tputc('\n');
  oflush();
  int pid = jobs[i].pid;
  job_remove(i);
  tty_set_fg(1, pid);
  while (!waitpid_nb(pid, &last_status))
    msleep(4);
  tty_set_fg(1, 0);
}

/* wait [job...]: block until the named jobs, or all of them, finish */
void bi_wait(int argc, char **argv)
{
  if (argc == 1) {
    while (njobs) {
      int status;
      if (waitpid_nb(jobs[0].pid, &status))
        job_remove(0);
      else
        msleep(4);
    }
    return;
  }
  for (int a = 1; a < argc; a++) {
    int i = job_find(argv[a]);
    if (i < 0) {
      err("wait: no such job: ", argv[a]);
      continue;
    }
    int pid = jobs[i].pid;
    while (!waitpid_nb(pid, &last_status))
      msleep(4);
    for (int k = 0; k < njobs; k++)
      if (jobs[k].pid == pid) {
        job_remove(k);
        break;
      }
  }
}

static int signal_number(const char *name)
{
  static const struct {
    const char *name;
    int number;
  } table[] = {{"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL}, {"TERM", SIGTERM}};
  if (name[0] >= '0' && name[0] <= '9')
    return (int)to_uint(name);
  if (name[0] == 'S' && name[1] == 'I' && name[2] == 'G')
    name += 3;
  for (unsigned i = 0; i < sizeof table / sizeof *table; i++)
    if (streq(name, table[i].name))
      return table[i].number;
  return -1;
}

/* kill [-SIGNAL] pid|%job... */
void bi_kill(int argc, char **argv)
{
  int sig = SIGTERM, first = 1;
  if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
    sig = signal_number(argv[1] + 1);
    first = 2;
    if (sig < 0) {
      err("kill: unknown signal: ", argv[1]);
      return;
    }
  }
  if (first >= argc) {
    tputs("usage: kill [-SIGNAL] pid|%job ...\n");
    return;
  }
  for (int a = first; a < argc; a++) {
    int pid;
    if (argv[a][0] == '%') {
      int i = job_find(argv[a]);
      pid = i < 0 ? -1 : jobs[i].pid;
    } else {
      pid = (int)to_uint(argv[a]);
    }
    if (pid <= 0 || kill(pid, sig) < 0)
      err("kill: no such process: ", argv[a]);
  }
}
