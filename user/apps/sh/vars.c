/* vars.c: identity, working directory, path lookup and shell variables */

#include "sh.h"

/* The shell's identity */
char username[32] = "root";

char home[256] = "/home/root";

char hostnm[64] = "owos";

int last_status; /* exit status of the last program, for $? */

/* Refresh `username`/`home` for the calling process */
void reload_identity(void) { user_ident(username, sizeof username, home, sizeof home); }

/* tiny string helpers */

bool has_prefix(const char *s, const char *p)
{
  while (*p) {
    if (*s++ != *p++)
      return false;
  }
  return true;
}

/* cwd + path resolution */

char cwd[256] = "/home/root";

/* Resolve `name` against cwd into `out`: ~ expands to the user's home, then  */
void abspath(const char *name, char *out)
{
  char in[512];
  if (name[0] == '~' && (name[1] == '\0' || name[1] == '/')) {
    strcpy(in, home);
    strcat(in, name + 1);
  } else if (name[0] == '/') {
    strcpy(in, name);
  } else {
    strcpy(in, cwd);
    int l = strlen(in);
    if (l == 0 || in[l - 1] != '/') {
      in[l] = '/';
      in[l + 1] = '\0';
    }
    strcat(in, name);
  }
  char seg[64][32];
  int ns = 0, i = 0;
  while (in[i]) {
    if (in[i] == '/') {
      i++;
      continue;
    }
    char s[32];
    int j = 0;
    while (in[i] && in[i] != '/' && j < 31)
      s[j++] = in[i++];
    s[j] = '\0';
    if (strcmp(s, ".") == 0)
      continue;
    if (strcmp(s, "..") == 0) {
      if (ns > 0)
        ns--;
      continue;
    }
    if (ns < 64)
      strcpy(seg[ns++], s);
  }
  if (ns == 0) {
    strcpy(out, "/");
    return;
  }
  out[0] = '\0';
  for (int k = 0; k < ns; k++) {
    strcat(out, "/");
    strcat(out, seg[k]);
  }
}

/* Keep USER, LOGNAME, HOME, SHELL and PWD in step with the shell's identity and directory */
void sync_env(void)
{
  setenv("USER", username, 1);
  setenv("LOGNAME", username, 1);
  setenv("HOME", home, 1);
  setenv("SHELL", "/bin/sh", 1);
  setenv("PWD", cwd, 1);
  setenv("PATH", "/bin", 0);
}

bool valid_var_name(const char *s, int len)
{
  if (len <= 0 || (s[0] >= '0' && s[0] <= '9'))
    return false;
  for (int i = 0; i < len; i++)
    if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
      return false;
  return true;
}

/* "NAME=value" with a valid NAME */
bool is_assignment(const char *w)
{
  int n = 0;
  while (w[n] && w[n] != '=')
    n++;
  return w[n] == '=' && valid_var_name(w, n);
}

void set_assignment(const char *w)
{
  char name[128];
  int n = 0;
  while (w[n] != '=' && n < (int)sizeof name - 1) {
    name[n] = w[n];
    n++;
  }
  name[n] = '\0';
  setenv(name, w + n + 1, 1);
}

unsigned to_uint(const char *s)
{
  unsigned v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (unsigned)(*s++ - '0');
  return v;
}

/* Find an executable: a name with a '/' is taken as given, anything else is searched for along PATH. */
bool find_command(const char *name, char *out, int cap)
{
  struct statinfo st;
  if (strchr(name, '/')) {
    abspath(name, out);
    return stat(out, &st) == 0 && !st.is_dir;
  }
  const char *path = getenv("PATH");
  char *dirs = malloc(strlen(path ? path : "/bin") + 1);
  if (!dirs)
    return false;
  strcpy(dirs, path ? path : "/bin");
  bool found = false;
  for (char *dir = dirs; dir && !found;) {
    char *colon = strchr(dir, ':');
    if (colon)
      *colon = '\0';
    if (strlen(dir) + strlen(name) + 2 <= (unsigned)cap) {
      strcpy(out, dir);
      strcat(out, "/");
      strcat(out, name);
      found = stat(out, &st) == 0 && !st.is_dir;
    }
    dir = colon ? colon + 1 : 0;
  }
  free(dirs);
  return found;
}
