/* sh.c: the OwOS shell. It talks to its terminal through stdin and stdout only: raw key bytes in (ESC [ A for the
 * arrows, ...), text and ANSI escapes out. /bin/term hosts it on a pty, sshd on a login pty. */

#include "sh.h"

static void bi_help(int, char **)
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

static void bi_clear(int, char **) { tputs("\x1b[2J\x1b[H"); }

static void bi_echo(int argc, char **argv)
{
  for (int k = 1; k < argc; k++) {
    tputs(argv[k]);
    if (k < argc - 1)
      tputc(' ');
  }
  tputc('\n');
}

static void bi_pwd(int, char **)
{
  tputs(cwd);
  tputc('\n');
}

static void bi_cd(int argc, char **argv)
{
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
}

static void bi_mkdir(int argc, char **argv)
{
  for (int k = 1; k < argc; k++) {
    char p[256];
    abspath(argv[k], p);
    int rc = mkdir(p);
    if (rc < 0)
      errc(rc, "mkdir", "failed: ", argv[k]);
  }
}

static void bi_rmdir(int argc, char **argv)
{
  for (int k = 1; k < argc; k++) {
    char p[256];
    abspath(argv[k], p);
    int rc = rmdir(p);
    if (rc < 0)
      errc(rc, "rmdir", "failed: ", argv[k]);
  }
}

static void bi_rm(int argc, char **argv)
{
  for (int k = 1; k < argc; k++) {
    char p[256];
    abspath(argv[k], p);
    int rc = unlink(p);
    if (rc < 0)
      errc(rc, "rm", "failed: ", argv[k]);
  }
}

static void bi_mv(int argc, char **argv)
{
  if (argc < 3) {
    tputs("usage: mv <src> <dst>\n");
    return;
  }
  char a[256], b[256];
  abspath(argv[1], a);
  abspath(argv[2], b);
  int rc = rename(a, b);
  if (rc < 0)
    errc(rc, "mv", "failed: ", argv[1]);
}

static void bi_exit(int, char **)
{
  oflush();
  exit(0);
}

static void bi_logout(int, char **) { do_login("root", 0); }

static void bi_source(int argc, char **argv)
{
  char file[256];
  if (argc < 2) {
    tputs("usage: source <file>\n");
    return;
  }
  abspath(argv[1], file);
  if (!run_file(file))
    err("source: cannot read ", argv[1]);
}

static void bi_storage(int, char **)
{
  if (!cat_proc("storage"))
    tputs("no storage devices\n");
}

static void bi_sleep(int argc, char **argv)
{
  if (argc > 1)
    msleep(to_uint(argv[1]) * 1000);
}

static void bi_poweroff(int, char **) { power(0); }

static void bi_restart(int, char **) { power(1); }

static void bi_zip(int argc, char **argv) { do_compress(argc, argv, ".z", "zip", zip_compress, zip_bound); }

static void bi_unzip(int argc, char **argv) { do_decompress(argc, argv, ".z", "unzip", zip_decompress, zip_orig_size); }

static void bi_gzip(int argc, char **argv) { do_compress(argc, argv, ".gz", "gzip", gzip_compress, gzip_bound); }

static void bi_gunzip(int argc, char **argv)
{
  do_decompress(argc, argv, ".gz", "gunzip", gzip_decompress, gzip_orig_size);
}

/* A builtin runs in the shell itself: fn, or with no fn the contents of /proc/<proc> */
struct builtin {
  const char *name;
  void (*fn)(int argc, char **argv);
  const char *proc;
};

static const struct builtin builtins[] = {
    {"help", bi_help},
    {"clear", bi_clear},
    {"echo", bi_echo},
    {"pwd", bi_pwd},
    {"cd", bi_cd},
    {"ls", bi_ls},
    {"cat", bi_cat},
    {"cp", bi_cp},
    {"mv", bi_mv},
    {"stat", bi_stat},
    {"mkdir", bi_mkdir},
    {"rmdir", bi_rmdir},
    {"rm", bi_rm},
    {"exit", bi_exit},
    {"uptime", bi_uptime},
    {"hostname", bi_hostname},
    {"whoami", bi_whoami},
    {"which", bi_which},
    {"umask", bi_umask},
    {"cpuinfo", 0, "cpuinfo"},
    {"meminfo", 0, "meminfo"},
    {"date", 0, "date"},
    {"journal", 0, "journal"},
    {"lsusb", 0, "usb"},
    {"ps", 0, "tasks"},
    {"storage", bi_storage},
    {"sleep", bi_sleep},
    {"id", cmd_id},
    {"useradd", cmd_useradd},
    {"userdel", cmd_userdel},
    {"passwd", cmd_passwd},
    {"usermod", cmd_usermod},
    {"groupadd", cmd_groupadd},
    {"groupdel", cmd_groupdel},
    {"users", cmd_users},
    {"groups", cmd_groups},
    {"su", cmd_su},
    {"login", cmd_login},
    {"logout", bi_logout},
    {"export", bi_export},
    {"unset", bi_unset},
    {"env", bi_env},
    {"printenv", bi_env},
    {"jobs", bi_jobs},
    {"fg", bi_fg},
    {"wait", bi_wait},
    {"kill", bi_kill},
    {"source", bi_source},
    {".", bi_source},
    {"poweroff", bi_poweroff},
    {"restart", bi_restart},
    {"loadkeys", bi_loadkeys},
    {"clip", bi_clip},
    {"mount", bi_mount},
    {"umount", bi_umount},
    {"services", bi_services},
    {"grep", bi_grep},
    {"zip", bi_zip},
    {"unzip", bi_unzip},
    {"gzip", bi_gzip},
    {"gunzip", bi_gunzip},
};

static const struct builtin *builtin_find(const char *name)
{
  for (unsigned i = 0; i < sizeof builtins / sizeof *builtins; i++)
    if (streq(name, builtins[i].name))
      return &builtins[i];
  return 0;
}

bool is_builtin(const char *name) { return builtin_find(name) != 0; }

void run_argv(int argc, char **argv)
{
  const struct builtin *b = builtin_find(argv[0]);
  if (!b)
    run_other(argc, argv, false);
  else if (b->fn)
    b->fn(argc, argv);
  else
    cat_proc(b->proc);
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
