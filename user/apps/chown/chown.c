/* chown.c - change file owner and group: chown [-R] OWNER[:GROUP] FILE... */
#include "idnames.h"
#include "opt.h"
#include "owos.h"
#include <stdbool.h>

static int new_uid = -1, new_gid = -1;
static bool recursive, failed;

static void report(const char *path, int rc)
{
  printf("chown: %s: %s\n", path,
         rc == -EACCES ? "permission denied"
         : rc == -1    ? "no such file or not supported"
                       : "failed");
  failed = true;
}

static void apply(const char *path)
{
  struct statinfo st;
  int rc = stat(path, &st);
  if (rc == 0)
    rc = chown(path, new_uid, new_gid);
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

static bool parse_spec(char *s)
{
  struct idnames names;
  unsigned id;
  bool ok = true;
  char *colon = s;
  while (*colon && *colon != ':')
    colon++;
  char *grp = *colon ? colon + 1 : NULL;
  *colon = '\0';
  idn_load(&names);
  if (*s) {
    if (idn_parse_user(&names, s, &id))
      new_uid = (int)id;
    else {
      printf("chown: invalid user: '%s'\n", s);
      ok = false;
    }
  }
  if (grp && *grp) {
    if (idn_parse_group(&names, grp, &id))
      new_gid = (int)id;
    else {
      printf("chown: invalid group: '%s'\n", grp);
      ok = false;
    }
  }
  idn_free(&names);
  return ok;
}

int main(int argc, char **argv)
{
  static const struct opt opts[] = {
      {'R', "recursive", OPT_FLAG, 'R'},
      {'h', "help", OPT_FLAG, 'h'},
  };
  struct optstate st;
  char *spec = NULL, **files = malloc((unsigned)(argc + 1) * sizeof *files);
  int nfiles = 0;
  if (!files)
    return 1;
  opt_init(&st, argc, argv);
  for (int c; (c = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (c == 'R') {
      recursive = true;
    } else if (c == 'h') {
      puts("usage: chown [-R] OWNER[:GROUP] FILE...\n");
      return 0;
    } else if (c == OPT_POS && !spec) {
      spec = (char *)st.arg;
    } else if (c == OPT_POS) {
      files[nfiles++] = (char *)st.arg;
    } else {
      printf("chown: invalid option '%s'\n", st.bad);
      return 1;
    }
  }
  if (!spec || nfiles == 0) {
    puts("usage: chown [-R] OWNER[:GROUP] FILE...\n");
    return 1;
  }
  char *copy = malloc((unsigned)strlen(spec) + 1);
  if (!copy)
    return 1;
  strcpy(copy, spec);
  if (!parse_spec(copy) || (new_uid < 0 && new_gid < 0))
    return 1;
  for (int i = 0; i < nfiles; i++)
    apply(files[i]);
  return failed ? 1 : 0;
}
