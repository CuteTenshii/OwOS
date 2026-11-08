/* sudo.c: run a command as root. A user in group "wheel" or "sudo" proves their own password once; the kernel then
 * lets that uid spawn root processes for a while (see users_sudo_grant), so later runs do not ask again. */

#include "opt.h"
#include "owos.h"
#include <stdbool.h>

enum { O_FORGET = 1, O_VALIDATE, O_HELP };

static const struct opt opts[] = {
    {'k', "reset-timestamp", OPT_FLAG, O_FORGET, 0, "forget the saved authentication and exit"},
    {'v', "validate", OPT_FLAG, O_VALIDATE, 0, "authenticate without running a command"},
    {'h', "help", OPT_FLAG, O_HELP, 0, "show this help"},
};

#define ATTEMPTS 3

/* One request to /proc/sudo: a command letter and an optional password */
static bool sudo_ctl(char command, const char *arg)
{
  char req[300];
  snprintf(req, sizeof req, "%c%s", command, arg);
  return writefile("/proc/sudo", req, (int)strlen(req)) >= 0;
}

/* Read a line from the terminal without echoing it */
static bool read_password(char *buf, int cap)
{
  int n = 0;
  for (;;) {
    char c;
    if (read(0, &c, 1) != 1)
      return false;
    if (c == '\r' || c == '\n')
      break;
    if ((c == 0x7f || c == '\b') && n > 0)
      n--;
    else if (c >= 0x20 && c != 0x7f && n < cap - 1)
      buf[n++] = c;
  }
  buf[n] = '\0';
  write(1, "\n", 1);
  return true;
}

static bool authenticate(void)
{
  char name[32], home[64];
  user_ident(name, sizeof name, home, sizeof home);
  if (!sudo_ctl('q', "")) {
    printf("sudo: %s is not allowed to run sudo (join group wheel or sudo)\n", name);
    return false;
  }
  if (sudo_ctl('?', ""))
    return true;
  for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
    char pass[200];
    printf("[sudo] password for %s: ", name);
    if (!read_password(pass, sizeof pass))
      return false;
    if (sudo_ctl('p', pass))
      return true;
    puts("Sorry, try again.\n");
  }
  printf("sudo: %d incorrect password attempts\n", ATTEMPTS);
  return false;
}

/* The command as one line for `sh -c`, every word single-quoted */
static char *command_line(int argc, char **argv)
{
  unsigned size = 1;
  for (int i = 0; i < argc; i++)
    size += (unsigned)strlen(argv[i]) * 4 + 3;
  char *line = malloc(size), *o = line;
  if (!line)
    return 0;
  for (int i = 0; i < argc; i++) {
    if (i)
      *o++ = ' ';
    *o++ = '\'';
    for (const char *c = argv[i]; *c; c++) {
      if (*c == '\'') { /* close the quote, escape the quote, reopen */
        *o++ = '\'';
        *o++ = '\\';
        *o++ = '\'';
      }
      *o++ = *c;
    }
    *o++ = '\'';
  }
  *o = '\0';
  return line;
}

int main(int argc, char **argv)
{
  bool forget = false, validate = false;
  char **rest = argv;
  int nrest = 0;
  struct optstate st;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (o == O_FORGET)
      forget = true;
    else if (o == O_VALIDATE)
      validate = true;
    else if (o == O_HELP) {
      opt_print_help("sudo", "COMMAND [ARG...]", "Run COMMAND as root.", opts, OPT_N(opts));
      return 0;
    } else if (o == OPT_POS) {
      /* the command starts at the first word; everything after it belongs to the command, options included */
      rest = argv + st.i - 1;
      nrest = argc - (st.i - 1);
      break;
    } else {
      printf("sudo: unknown option %s\n", st.bad);
      opt_print_usage("sudo", "COMMAND [ARG...]", opts, OPT_N(opts));
      return 1;
    }
  }
  if (forget) {
    sudo_ctl('k', "");
    return 0;
  }
  if (!nrest && !validate) {
    opt_print_usage("sudo", "COMMAND [ARG...]", opts, OPT_N(opts));
    return 1;
  }
  if (getuid() != 0 && !authenticate())
    return 1;
  if (validate)
    return 0;

  char *line = command_line(nrest, rest);
  if (!line)
    return 1;
  char *shell_argv[] = {"sh", "-c", line};
  struct spawn_opts o = {.path = "/bin/sh",
                         .argv = shell_argv,
                         .argc = 3,
                         .flags = SPAWN_STDIO | SPAWN_CREDS,
                         .uid = 0,
                         .gid = 0,
                         .stdio = {0, 1, 2}};
  int pid = spawn_ex(&o);
  free(line);
  if (pid < 0) {
    puts("sudo: cannot run the command as root\n");
    return 1;
  }
  int self = tty_fg(1);
  tty_set_fg(1, pid); /* Ctrl+C goes to the command, not to us */
  int status = waitpid(pid);
  tty_set_fg(1, self);
  return status;
}
