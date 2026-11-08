/* chmod.c - change file modes: chmod [-R] MODE FILE... (octal or symbolic MODE) */
#include "modeutil.h"
#include "opt.h"
#include "owos.h"
#include <stdbool.h>

static const char *spec;
static bool recursive, failed;
static unsigned cur_umask;

static void report(const char *path, int rc)
{
  printf("chmod: %s: %s\n", path,
         rc == -EACCES ? "permission denied"
         : rc == -1    ? "no such file or not supported"
                       : "failed");
  failed = true;
}

static void apply(const char *path)
{
  struct statinfo st;
  unsigned mode;
  int rc = stat(path, &st);
  if (rc != 0) {
    report(path, rc);
    return;
  }
  if (!mode_parse(spec, st.mode, cur_umask, st.is_dir, &mode)) {
    printf("chmod: invalid mode '%s'\n", spec);
    exit(1);
  }
  rc = chmod(path, mode);
  if (rc != 0) {
    report(path, rc);
    return;
  }
  if (!recursive || !st.is_dir)
    return;
  struct dirent_u de;
  for (int i = 0; readdir(path, i, &de) == 1; i++) {
    int plen = strlen(path);
    char *child = malloc((unsigned)(plen + strlen(de.name) + 2));
    if (!child)
      return;
    strcpy(child, path);
    if (plen && path[plen - 1] != '/')
      strcat(child, "/");
    strcat(child, de.name);
    apply(child);
    free(child);
  }
}

int main(int argc, char **argv)
{
  static const struct opt opts[] = {
      {'R', "recursive", OPT_FLAG, 'R', 0, "change files and directories recursively"},
      {'h', "help", OPT_FLAG, 'h', 0, "show this help"},
  };
  struct optstate st;
  unsigned probe;
  int nfiles = 0;
  char **files = malloc((unsigned)(argc + 1) * sizeof *files);
  if (!files)
    return 1;
  opt_init(&st, argc, argv);
  for (int c; (c = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (c == 'R') {
      recursive = true;
    } else if (c == 'h') {
      opt_print_help("chmod", "MODE FILE...", "Set the permission bits of each FILE (octal or symbolic MODE).", opts,
                     OPT_N(opts));
      return 0;
    } else if (c == OPT_POS && !spec) {
      spec = st.arg;
    } else if (c == OPT_POS) {
      files[nfiles++] = (char *)st.arg;
    } else if (c == OPT_BAD && !spec && mode_parse(st.bad, 0, 0, false, &probe)) {
      spec = st.bad; /* a symbolic mode such as -w looks like an option */
    } else {
      printf("chmod: invalid option '%s'\n", st.bad);
      return 1;
    }
  }
  if (!spec || nfiles == 0) {
    opt_print_usage("chmod", "MODE FILE...", opts, OPT_N(opts));
    return 1;
  }
  cur_umask = umask((unsigned)-1);
  for (int i = 0; i < nfiles; i++)
    apply(files[i]);
  return failed ? 1 : 0;
}
