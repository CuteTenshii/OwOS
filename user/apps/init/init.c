/* init.c — PID-1 init / service manager, in user space.
 *
 * The kernel launches this one process at boot; it brings up everything else.
 * Reads /etc/inittab (one /bin program per line, optional args; '#' comments
 * and blank lines ignored), spawns each detached as root (init runs as root),
 * then stays alive as the supervisor: it blocks in waitpid reaping exited
 * children so daemons don't linger as zombies. It never exits. No respawn: an
 * inittab daemon that exits (dhcpcd is one-shot after it leases) stays down --
 * restart it by hand -- rather than being relaunched in a tight loop. */

#include "owos.h"

#define LINE_MAX 128

/* Split "prog arg" in place: returns the program name and stores the single arg
 * (or NULL) in *arg. */
static const char *split(char *line, const char **arg)
{
  char *p = line;
  while (*p && *p != ' ' && *p != '\t')
    p++;
  if (*p) {
    *p++ = '\0';
    while (*p == ' ' || *p == '\t')
      p++;
    *arg = *p ? p : 0;
  } else {
    *arg = 0;
  }
  return line;
}

/* Launch "prog [arg]" detached; a bare name resolves under /bin. */
static void launch(char *line)
{
  const char *arg;
  const char *name = split(line, &arg);

  char path[LINE_MAX + 8];
  if (name[0] == '/')
    snprintf(path, sizeof path, "%s", name);
  else
    snprintf(path, sizeof path, "/bin/%s", name);

  int pid = spawn(path, arg);
  if (pid < 0)
    printf("init: failed to spawn %s\n", path);
  else
    printf("init: started %s (pid %d)\n", path, pid);
}

int main(void)
{
  printf("init: pid 1 up, reading /etc/inittab\n");

  char buf[2048];
  int len = readfile("/etc/inittab", buf, (int)sizeof buf - 1);
  if (len < 0)
    len = 0;
  buf[len] = '\0';

  /* Spawn one entry per non-blank, non-comment line. */
  int start = 0;
  for (int i = 0; i <= len; i++) {
    if (buf[i] != '\n' && buf[i] != '\0')
      continue;
    buf[i] = '\0';
    char *line = &buf[start];
    start = i + 1;
    while (*line == ' ' || *line == '\t')
      line++;
    if (*line == '\0' || *line == '#')
      continue;
    launch(line);
  }

  /* Supervise: block in waitpid reaping any child that exits. Blocking (not
   * spinning) is the point -- init is idle whenever the system is healthy, and
   * must never exit (nothing else supervises). */
  for (;;) {
    if (waitpid(-1) < 0)
      msleep(1000); /* no children (empty inittab / all failed): idle */
  }
  return 0;
}
