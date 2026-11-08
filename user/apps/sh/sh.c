/* sh.c: the OwOS shell. It talks to its terminal through stdin and stdout only: raw key bytes in (ESC [ A for the
 * arrows, ...), text and ANSI escapes out. /bin/term hosts it on a pty, sshd on a login pty. */

#include "sh.h"

static void bi_help(void)
{
  tputs_c("OwOS shell.  Ctrl+C cancels the line / aborts a command.\n", FG_DIR);
  tputs("files    ls cat cp mv rm mkdir rmdir stat chmod chown umask pwd cd grep\n");
  tputs("archive  zip unzip gzip gunzip\n");
  tputs("system   uptime hostname whoami id cpuinfo meminfo date ps journal "
        "which\n");
  tputs("users    useradd userdel passwd usermod groupadd groupdel users "
        "groups\n");
  tputs("session  su login logout    (su alice [password]; logout -> root)\n");
  tputs("device   storage lsusb mount umount loadkeys clip\n");
  tputs("net      services (dhcp/ntp run as services: `services start ntp`)\n");
  tputs("power    poweroff restart\n");
  tputs("shell    help clear echo sleep source exit    (~/.shrc runs at startup)\n");
  tputs("jobs     cmd &  jobs fg wait kill      export unset env printenv\n");
  tputs("root     sudo <command>    (group wheel or sudo)\n");
  tputs("remote   ssh user@host (interactive SSH-2 session)\n");
  tputs("Any /bin program runs here: its output streams inline; if it opens a\n");
  tputs("window (editor files clock browser ...) the prompt returns.\n");
}

static void bi_export(int argc, char **argv)
{
  for (int i = 1; i < argc; i++) {
    if (is_assignment(argv[i]))
      set_assignment(argv[i]);
    else if (!valid_var_name(argv[i], (int)strlen(argv[i])))
      err("export: not a valid name: ", argv[i]);
  }
}

static void bi_unset(int argc, char **argv)
{
  for (int i = 1; i < argc; i++)
    unsetenv(argv[i]);
}

/* env lists every variable; printenv NAME prints one value */
static void bi_env(int argc, char **argv)
{
  if (argc > 1) {
    const char *v = getenv(argv[1]);
    if (v) {
      tputs(v);
      tputc('\n');
    }
    return;
  }
  char entry[1024];
  for (unsigned i = 0; env_entry(i, entry, sizeof entry) >= 0; i++) {
    tputs(entry);
    tputc('\n');
  }
}

static const char *const builtin_names[] = {
    "help",     "clear", "echo",    "pwd",   "cd",     "ls",       "cat",     "cp",     "mv",      "stat",
    "mkdir",    "rmdir", "rm",      "exit",  "uptime", "hostname", "whoami",  "which",  "umask",   "cpuinfo",
    "meminfo",  "date",  "storage", "sleep", "id",     "useradd",  "userdel", "passwd", "usermod", "groupadd",
    "groupdel", "users", "groups",  "su",    "login",  "logout",   "export",  "unset",  "env",     "printenv",
    "jobs",     "fg",    "wait",    "kill",  "source", ".",
};

bool is_builtin(const char *name)
{
  for (unsigned i = 0; i < sizeof builtin_names / sizeof *builtin_names; i++)
    if (streq(name, builtin_names[i]))
      return true;
  return false;
}

void run_argv(int argc, char **argv)
{
  const char *c = argv[0];

  if (!strcmp(c, "help"))
    bi_help();
  else if (!strcmp(c, "clear"))
    tputs("\x1b[2J\x1b[H");
  else if (!strcmp(c, "echo")) {
    for (int k = 1; k < argc; k++) {
      tputs(argv[k]);
      if (k < argc - 1)
        tputc(' ');
    }
    tputc('\n');
  } else if (!strcmp(c, "pwd")) {
    tputs(cwd);
    tputc('\n');
  } else if (!strcmp(c, "cd")) {
    const char *target = argc < 2 ? getenv("HOME") : streq(argv[1], "-") ? getenv("OLDPWD") : argv[1];
    char p[256], previous[256];
    strcpy(previous, cwd);
    abspath(target ? target : home, p);
    struct statinfo st;
    int rc = stat(p, &st);
    if (rc == 0 && st.is_dir && (rc = path_access(p, X_OK)) == 0) {
      strcpy(cwd, p);
      chdir(cwd); /* programs we start inherit it */
      setenv("OLDPWD", previous, 1);
      setenv("PWD", cwd, 1);
    } else {
      errc(rc, "cd", "not a directory: ", target ? target : home);
    }
  } else if (!strcmp(c, "ls"))
    bi_ls(argc, argv);
  else if (!strcmp(c, "cat"))
    bi_cat(argc, argv);
  else if (!strcmp(c, "cp"))
    bi_cp(argc, argv);
  else if (!strcmp(c, "stat"))
    bi_stat(argc, argv);
  else if (!strcmp(c, "umask"))
    bi_umask(argc, argv);
  else if (!strcmp(c, "mkdir")) {
    for (int k = 1; k < argc; k++) {
      char p[256];
      abspath(argv[k], p);
      int rc = mkdir(p);
      if (rc < 0)
        errc(rc, "mkdir", "failed: ", argv[k]);
    }
  } else if (!strcmp(c, "rmdir")) {
    for (int k = 1; k < argc; k++) {
      char p[256];
      abspath(argv[k], p);
      int rc = rmdir(p);
      if (rc < 0)
        errc(rc, "rmdir", "failed: ", argv[k]);
    }
  } else if (!strcmp(c, "rm")) {
    for (int k = 1; k < argc; k++) {
      char p[256];
      abspath(argv[k], p);
      int rc = unlink(p);
      if (rc < 0)
        errc(rc, "rm", "failed: ", argv[k]);
    }
  } else if (!strcmp(c, "mv")) {
    if (argc < 3)
      tputs("usage: mv <src> <dst>\n");
    else {
      char a[256], b[256];
      abspath(argv[1], a);
      abspath(argv[2], b);
      int rc = rename(a, b);
      if (rc < 0)
        errc(rc, "mv", "failed: ", argv[1]);
    }
  } else if (!strcmp(c, "exit")) {
    oflush();
    exit(0);
  } else if (!strcmp(c, "uptime"))
    bi_uptime();
  else if (!strcmp(c, "whoami"))
    bi_whoami();
  else if (!strcmp(c, "hostname"))
    bi_hostname(argc, argv);
  else if (!strcmp(c, "useradd"))
    cmd_useradd(argc, argv);
  else if (!strcmp(c, "userdel"))
    cmd_userdel(argc, argv);
  else if (!strcmp(c, "passwd"))
    cmd_passwd(argc, argv);
  else if (!strcmp(c, "groupadd"))
    cmd_groupadd(argc, argv);
  else if (!strcmp(c, "groupdel"))
    cmd_groupdel(argc, argv);
  else if (!strcmp(c, "usermod"))
    cmd_usermod(argc, argv);
  else if (!strcmp(c, "users"))
    cmd_users();
  else if (!strcmp(c, "groups"))
    cmd_groups(argc, argv);
  else if (!strcmp(c, "id"))
    cmd_id(argc, argv);
  else if (!strcmp(c, "su"))
    cmd_su(argc, argv);
  else if (!strcmp(c, "login"))
    cmd_login(argc, argv);
  else if (!strcmp(c, "logout"))
    do_login("root", 0);
  else if (!strcmp(c, "which"))
    bi_which(argc, argv);
  else if (!strcmp(c, "export"))
    bi_export(argc, argv);
  else if (!strcmp(c, "unset"))
    bi_unset(argc, argv);
  else if (!strcmp(c, "env") || !strcmp(c, "printenv"))
    bi_env(argc, argv);
  else if (!strcmp(c, "source") || !strcmp(c, ".")) {
    char file[256];
    if (argc < 2) {
      tputs("usage: source <file>\n");
    } else {
      abspath(argv[1], file);
      if (!run_file(file))
        err("source: cannot read ", argv[1]);
    }
  } else if (!strcmp(c, "jobs"))
    bi_jobs();
  else if (!strcmp(c, "fg"))
    bi_fg(argc, argv);
  else if (!strcmp(c, "wait"))
    bi_wait(argc, argv);
  else if (!strcmp(c, "kill"))
    bi_kill(argc, argv);
  else if (!strcmp(c, "meminfo"))
    cat_proc("meminfo");
  else if (!strcmp(c, "cpuinfo"))
    cat_proc("cpuinfo");
  else if (!strcmp(c, "date"))
    cat_proc("date");
  else if (!strcmp(c, "storage")) {
    if (!cat_proc("storage"))
      tputs("no storage devices\n");
  } else if (!strcmp(c, "sleep")) {
    if (argc > 1)
      msleep(to_uint(argv[1]) * 1000);
  } else if (!strcmp(c, "journal"))
    cat_proc("journal");
  else if (!strcmp(c, "poweroff"))
    power(0);
  else if (!strcmp(c, "restart"))
    power(1);
  else if (!strcmp(c, "loadkeys"))
    bi_loadkeys(argc, argv);
  else if (!strcmp(c, "clip"))
    bi_clip(argc, argv);
  else if (!strcmp(c, "mount"))
    bi_mount(argc, argv);
  else if (!strcmp(c, "umount"))
    bi_umount(argc, argv);
  else if (!strcmp(c, "services"))
    bi_services(argc, argv);
  else if (!strcmp(c, "lsusb"))
    cat_proc("usb");
  else if (!strcmp(c, "ps"))
    cat_proc("tasks");
  else if (!strcmp(c, "grep"))
    bi_grep(argc, argv);
  else if (!strcmp(c, "zip"))
    do_compress(argc, argv, ".z", "zip", zip_compress, zip_bound);
  else if (!strcmp(c, "unzip"))
    do_decompress(argc, argv, ".z", "unzip", zip_decompress, zip_orig_size);
  else if (!strcmp(c, "gzip"))
    do_compress(argc, argv, ".gz", "gzip", gzip_compress, gzip_bound);
  else if (!strcmp(c, "gunzip"))
    do_decompress(argc, argv, ".gz", "gunzip", gzip_decompress, gzip_orig_size);
  else
    run_other(argc, argv, false);
}

int main(int argc, char **argv)
{
  reload_identity();
  gethostname(hostnm, sizeof hostnm);
  struct statinfo st;
  const char *pwd = getenv("PWD"); /* a shell started from another keeps its directory */
  if (pwd && pwd[0] == '/' && stat(pwd, &st) == 0 && st.is_dir)
    strlcpy(cwd, pwd, sizeof cwd);
  else
    strcpy(cwd, home);
  chdir(cwd);
  sync_env();

  if (argc >= 3 && streq(argv[1], "-c")) {
    run_line(argv[2]);
    oflush();
    return 0;
  }

  if (isatty(0)) { /* the startup files of an interactive shell */
    char rc[300];
    run_file("/etc/shrc");
    snprintf(rc, sizeof rc, "%s/.shrc", home);
    run_file(rc);
  }

  if (isatty(0))
    tputs("OwOS shell -- type 'help'.\n");
  for (;;) {
    prompt();
    if (!edit_line())
      break;
    history_add();
    if (line)
      run_line(line);
    bg_service();
  }
  oflush();
  return 0;
}
