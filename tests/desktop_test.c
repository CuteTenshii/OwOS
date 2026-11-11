/* Host test for the Desktop Entry library (desktop.c) and the shipped .desktop files. */
#define OWOS_SUITE_TEST
#define _GNU_SOURCE
#include "desktop.h"
#include "desktop_fs.h"
#include "test.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *g_dir;
static const char *const *g_names;

int desktop_test_readdir(const char *path, int idx, struct dirent_u *out)
{
  if (!g_dir || strcmp(path, g_dir) != 0)
    return 0;
  for (int i = 0; i <= idx; i++)
    if (!g_names[i])
      return 0;
  const char *n = g_names[idx];
  size_t len = strlen(n);
  out->is_dir = len && n[len - 1] == '/';
  snprintf(out->name, sizeof out->name, "%.*s", (int)len - out->is_dir, n);
  return 1;
}

#define ENTRY(body) "[Desktop Entry]\nType=Application\n" body
#define PARSE(s) desktop_parse((s), (int)strlen(s), "id")

static struct desktop_entry *parse_ok(const char *s)
{
  struct desktop_entry *e = PARSE(s);
  expect(e != NULL);
  return e;
}

static void check_basic(void)
{
  static const char full[] =
      "# comment\n\n[Desktop Entry]\r\nType=Application\r\nName = Foo App \nGenericName=Gen\nComment=C\nExec=foo %f\n"
      "TryExec=foo\nPath=/home\nIcon=foo-icon\nTerminal=true\nNoDisplay=true\nStartupWMClass=FooWM\n"
      "X-OwOS-Order=-5\nCategories=A;B;\nKeywords=k1;k2\nMimeType=text/plain;\nOnlyShowIn=OwOS;\nNotShowIn=X;\n"
      "X-Unknown=1\n";
  struct desktop_entry *e = desktop_parse(full, (int)sizeof full - 1, "foo");
  expect(e != NULL);
  if (!e)
    return;
  expect_string(e->id, "foo");
  expect_string(e->name, "Foo App");
  expect_string(e->generic_name, "Gen");
  expect_string(e->comment, "C");
  expect_string(e->exec, "foo %f");
  expect_string(e->try_exec, "foo");
  expect_string(e->path, "/home");
  expect_string(e->icon, "foo-icon");
  expect_string(e->wm_class, "FooWM");
  expect(e->terminal && e->no_display && !e->hidden);
  expect(e->order == -5);
  expect(e->categories.n == 2 && !strcmp(e->categories.v[1], "B"));
  expect(e->keywords.n == 2 && !strcmp(e->keywords.v[1], "k2"));
  expect(e->mime_types.n == 1);
  expect(e->only_show_in.n == 1 && e->not_show_in.n == 1);
  desktop_free(e);

  e = parse_ok(ENTRY("Name=N\nExec=x\n"));
  expect(e && e->order == DESKTOP_ORDER_DEFAULT && !e->terminal && !e->icon);
  desktop_free(e);
  e = parse_ok(ENTRY("Name=N\nExec=x\nX-OwOS-Order=abc\n"));
  expect(e && e->order == DESKTOP_ORDER_DEFAULT);
  desktop_free(e);
  e = parse_ok(ENTRY("Name=N\nExec=x\nTerminal=True\n"));
  expect(e && !e->terminal);
  desktop_free(e);
}

static void check_escapes(void)
{
  struct desktop_entry *e = parse_ok(ENTRY("Name=a\\sb\\tc\\nd\\re\\\\f\nExec=x\nComment=semi\\;colon\n"
                                           "Keywords=one\\;two;three;;\\;\n"));
  if (!e)
    return;
  expect_string(e->name, "a b\tc\nd\re\\f");
  expect_string(e->comment, "semi;colon");
  expect(e->keywords.n == 3);
  expect_string(e->keywords.v[0], "one;two");
  expect_string(e->keywords.v[1], "three");
  expect_string(e->keywords.v[2], ";");
  desktop_free(e);
  e = parse_ok(ENTRY("Name=a\\qb\nExec=x\n"));
  expect(e && !strcmp(e->name, "a\\qb"));
  desktop_free(e);
}

static void check_localized(void)
{
  struct desktop_entry *e =
      parse_ok(ENTRY("Name[fr]=Francais\nName=Plain\nName[de_DE@euro]=Deutsch\nComment[fr]=Seulement\nExec=x\n"));
  if (!e)
    return;
  expect_string(e->name, "Plain");
  expect(e->comment == NULL);
  desktop_free(e);
  e = PARSE(ENTRY("Name[fr]=Only\nExec=x\n"));
  expect(e == NULL);
}

static void check_groups(void)
{
  struct desktop_entry *e = parse_ok(ENTRY("Name=N\nExec=x\nActions=new;ghost;nameless;\n"
                                           "[Desktop Action new]\nName=New Window\nExec=x --new %u\nIcon=i\n"
                                           "[Desktop Action unlisted]\nName=U\nExec=u\n"
                                           "[Desktop Action nameless]\nExec=n\n"
                                           "[Desktop Action new]\nName=Dup\n"
                                           "[X-Other]\nName=Other\nExec=other\n"
                                           "[Desktop Entry]\nName=Second\n"));
  if (!e)
    return;
  expect_string(e->name, "N");
  expect(e->nactions == 1);
  if (e->nactions == 1) {
    expect_string(e->actions[0].id, "new");
    expect_string(e->actions[0].name, "New Window");
    expect_string(e->actions[0].exec, "x --new %u");
    expect_string(e->actions[0].icon, "i");
  }
  desktop_free(e);

  e = parse_ok(ENTRY("Name=N\nExec=x\n[Desktop Action new]\nName=A\n"));
  expect(e && e->nactions == 0);
  desktop_free(e);

  e = PARSE("Type=Application\nName=N\nExec=x\n");
  expect(e == NULL);
  e = PARSE("[X-Foo]\nName=N\n[Desktop Entry]\nType=Application\nName=N\nExec=x\n");
  expect(e != NULL);
  desktop_free(e);
}

static void check_malformed(void)
{
  struct desktop_entry *e = PARSE("[Desktop Entry\nType=Application\nName=N\nExec=x\n");
  expect(e == NULL);
  e = parse_ok(ENTRY("garbage line\n=novalue\nbad key=1\nNam e=2\nName=N\nExec=x\n[broken\nName=Evil\n"));
  if (e) {
    expect_string(e->name, "N");
    desktop_free(e);
  }
  e = PARSE(ENTRY("Name=First\nName=Second\nExec=x\nExec=y\n"));
  expect(e && !strcmp(e->name, "First") && !strcmp(e->exec, "x"));
  desktop_free(e);
  expect(desktop_parse("", 0, "x") == NULL);
  expect(desktop_parse(NULL, 0, "x") == NULL);
  e = desktop_parse(ENTRY("Name=N\nExec=x"), (int)strlen(ENTRY("Name=N\nExec=x")), NULL);
  expect(e && !strcmp(e->id, ""));
  desktop_free(e);
  const char *no_nl = ENTRY("Name=N\nExec=x\nIcon=trunc");
  e = desktop_parse(no_nl, (int)strlen(no_nl) - 2, "x");
  expect(e && !strcmp(e->icon, "tru"));
  desktop_free(e);

  size_t big = 1 << 20;
  char *s = malloc(big + 128);
  strcpy(s, ENTRY("Exec=x\nName="));
  size_t off = strlen(s);
  memset(s + off, 'A', big);
  strcpy(s + off + big, "\n");
  e = desktop_parse(s, (int)strlen(s), "big");
  expect(e && strlen(e->name) == big);
  desktop_free(e);
  free(s);
}

static void check_validity(void)
{
  expect(PARSE("[Desktop Entry]\nType=Application\nExec=x\n") == NULL);
  expect(PARSE("[Desktop Entry]\nType=Application\nName=N\n") == NULL);
  expect(PARSE("[Desktop Entry]\nName=N\nExec=x\n") == NULL);
  expect(PARSE("[Desktop Entry]\nType=Link\nName=N\nExec=x\n") == NULL);
  expect(PARSE("[Desktop Entry]\nType=Directory\nName=N\nExec=x\n") == NULL);
  expect(PARSE(ENTRY("Name=N\nExec=x\nHidden=true\n")) == NULL);
  struct desktop_entry *e = parse_ok(ENTRY("Name=N\nExec=x\nHidden=false\n"));
  expect(e && !e->hidden);
  desktop_free(e);
}

static char *argv_join(char **argv, int argc)
{
  static char out[512];
  out[0] = '\0';
  for (int i = 0; i < argc; i++)
    snprintf(out + strlen(out), sizeof out - strlen(out), "%s[%s]", i ? " " : "", argv[i]);
  return out;
}

static const char *run_exec(const char *exec, const char *const *files, int nfiles)
{
  char buf[512];
  snprintf(buf, sizeof buf, "[Desktop Entry]\nType=Application\nName=My App\nIcon=ic\nExec=%s\n", exec);
  struct desktop_entry *e = desktop_parse(buf, (int)strlen(buf), "id");
  static char res[512];
  char **argv;
  int argc;
  if (!e)
    return "PARSE";
  e->location = strdup("/loc/id.desktop");
  int rc = desktop_exec_argv(e, NULL, files, nfiles, &argv, &argc);
  if (rc < 0) {
    desktop_free(e);
    return "ERR";
  }
  expect(argv[argc] == NULL);
  snprintf(res, sizeof res, "%s", argv_join(argv, argc));
  desktop_argv_free(argv, argc);
  desktop_free(e);
  return res;
}

static const char *two[] = {"/a b", "/c"};

static void exec_splitting(void)
{
  expect_string(run_exec("prog", NULL, 0), "[prog]");
  expect_string(run_exec("prog  a\tb   c", NULL, 0), "[prog] [a] [b] [c]");
  expect_string(run_exec("prog \"a b\" c", NULL, 0), "[prog] [a b] [c]");
  expect_string(run_exec("prog \"\" c", NULL, 0), "[prog] [] [c]");
  expect_string(run_exec("prog a\"b c\"d", NULL, 0), "[prog] [ab cd]");
  expect_string(run_exec("prog \"q\\\\\\\\x\" \"\\\\\"\"", NULL, 0), "[prog] [q\\x] [\"]");
  expect_string(run_exec("prog \"\\\\$HOME\"", NULL, 0), "[prog] [$HOME]");
  expect_string(run_exec("prog a\\\\ b", NULL, 0), "[prog] [a b]");
  expect_string(run_exec("prog a\\\\", NULL, 0), "ERR");
  expect_string(run_exec("prog \"unterminated", NULL, 0), "ERR");
  expect_string(run_exec("", NULL, 0), "PARSE");
  expect_string(run_exec("%f", NULL, 0), "ERR");
}

static void exec_field_codes(void)
{
  expect_string(run_exec("p %f", two, 2), "[p] [/a b]");
  expect_string(run_exec("p %u", two, 2), "[p] [/a b]");
  expect_string(run_exec("p %F", two, 2), "[p] [/a b] [/c]");
  expect_string(run_exec("p %U x", two, 2), "[p] [/a b] [/c] [x]");
  expect_string(run_exec("p %f", NULL, 0), "[p]");
  expect_string(run_exec("p %F", NULL, 0), "[p]");
  expect_string(run_exec("p --file=%f", NULL, 0), "[p] [--file=]");
  expect_string(run_exec("p --file=%f", two, 1), "[p] [--file=/a b]");
  expect_string(run_exec("p 100%%", NULL, 0), "[p] [100%]");
  expect_string(run_exec("p %%f", two, 1), "[p] [%f]");
  expect_string(run_exec("p %i", NULL, 0), "[p] [--icon] [ic]");
  expect_string(run_exec("p %c", NULL, 0), "[p] [My App]");
  expect_string(run_exec("p \"%c\"", NULL, 0), "[p] [My App]");
  expect_string(run_exec("p %k", NULL, 0), "[p] [/loc/id.desktop]");
  expect_string(run_exec("p %d %D %n %N %v %m x", NULL, 0), "[p] [x]");
}

static void exec_invalid_codes(void)
{
  expect_string(run_exec("p %z", NULL, 0), "ERR");
  expect_string(run_exec("p %", NULL, 0), "ERR");
  expect_string(run_exec("p x%F", two, 2), "ERR");
  expect_string(run_exec("p \"%F\"", two, 2), "ERR");
  expect_string(run_exec("p %Fx", two, 2), "ERR");
  expect_string(run_exec("p x%i", NULL, 0), "ERR");
  expect_string(run_exec("p %1", NULL, 0), "ERR");
}

static void exec_actions(void)
{
  struct desktop_entry *e =
      parse_ok(ENTRY("Name=N\nExec=main\nActions=a;\n[Desktop Action a]\nName=A\nExec=act --x %u\n"));
  if (!e)
    return;
  char **argv;
  int argc;
  expect(desktop_exec_argv(e, &e->actions[0], two, 1, &argv, &argc) == 0);
  expect_string(argv_join(argv, argc), "[act] [--x] [/a b]");
  desktop_argv_free(argv, argc);
  expect(desktop_exec_argv(e, NULL, NULL, 0, &argv, &argc) == 0);
  expect_string(argv_join(argv, argc), "[main]");
  desktop_argv_free(argv, argc);
  desktop_free(e);
  e = parse_ok(ENTRY("Name=N\nExec=main\nActions=a;\n[Desktop Action a]\nName=A\n"));
  expect(e && desktop_exec_argv(e, &e->actions[0], NULL, 0, &argv, &argc) < 0);
  desktop_free(e);
}

static bool visible(const char *body)
{
  struct desktop_entry *e = parse_ok(body);
  bool v = e && desktop_visible(e);
  desktop_free(e);
  return v;
}

static void check_visible(void)
{
  expect(visible(ENTRY("Name=N\nExec=x\n")));
  expect(!visible(ENTRY("Name=N\nExec=x\nNoDisplay=true\n")));
  expect(visible(ENTRY("Name=N\nExec=x\nOnlyShowIn=GNOME;OwOS;\n")));
  expect(!visible(ENTRY("Name=N\nExec=x\nOnlyShowIn=GNOME;KDE;\n")));
  expect(!visible(ENTRY("Name=N\nExec=x\nNotShowIn=OwOS;\n")));
  expect(visible(ENTRY("Name=N\nExec=x\nNotShowIn=GNOME;\n")));
  expect(!visible(ENTRY("Name=N\nExec=x\nOnlyShowIn=owos;\n")));
  char self[PATH_MAX], body[PATH_MAX + 128];
  expect(realpath(__FILE__, self) != NULL);
  snprintf(body, sizeof body, ENTRY("Name=N\nExec=x\nTryExec=%s\n"), self);
  expect(visible(body));
  expect(!visible(ENTRY("Name=N\nExec=x\nTryExec=/definitely/not/here\n")));
  expect(!visible(ENTRY("Name=N\nExec=x\nTryExec=nonexistent-program-xyz\n")));
}

static void check_sort(void)
{
  const char *specs[] = {ENTRY("Name=zeta\nExec=x\nX-OwOS-Order=1\n"),
                         ENTRY("Name=beta\nExec=x\n"),
                         ENTRY("Name=Alpha\nExec=x\n"),
                         ENTRY("Name=omega\nExec=x\nX-OwOS-Order=-3\n"),
                         ENTRY("Name=alpha2\nExec=x\n"),
                         ENTRY("Name=gamma\nExec=x\nX-OwOS-Order=1\n")};
  struct desktop_list l = {0};
  for (int i = 0; i < 6; i++) {
    l.v = realloc(l.v, (size_t)(l.n + 1) * sizeof *l.v);
    l.v[l.n++] = parse_ok(specs[i]);
  }
  desktop_sort(&l);
  const char *want[] = {"omega", "gamma", "zeta", "Alpha", "alpha2", "beta"};
  for (int i = 0; i < 6; i++)
    expect_string(l.v[i]->name, want[i]);
  desktop_list_free(&l);
  expect(l.n == 0 && l.v == NULL);
}

static void write_file(const char *dir, const char *name, const char *body)
{
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  expect(f != NULL);
  if (f) {
    fputs(body, f);
    fclose(f);
  }
}

static void check_scan(void)
{
  char a[256], b[256];
  const char *tmp = getenv("TMPDIR");
  snprintf(a, sizeof a, "%s/owos-desktop-XXXXXX", tmp ? tmp : "/tmp");
  if (!mkdtemp(a)) {
    expect(0);
    return;
  }
  snprintf(b, sizeof b, "%s/b", a);
  mkdir(b, 0755);

  write_file(a, "one.desktop", ENTRY("Name=One\nExec=one\nX-OwOS-Order=2\n"));
  write_file(a, "two.desktop", ENTRY("Name=Two\nExec=two\nX-OwOS-Order=1\n"));
  write_file(a, "hidden.desktop", ENTRY("Name=H\nExec=h\nHidden=true\n"));
  write_file(a, "bad.desktop", "not a desktop file\n");
  write_file(a, "readme.txt", ENTRY("Name=Txt\nExec=t\n"));
  write_file(a, "sub.desktop", ENTRY("Name=Sub\nExec=s\n"));
  write_file(a, ".desktop", ENTRY("Name=Dot\nExec=d\n"));
  write_file(b, "one.desktop", ENTRY("Name=One Override\nExec=one2\n"));
  write_file(b, "three.desktop", ENTRY("Name=Three\nExec=three\nX-OwOS-Order=3\n"));

  struct desktop_list l = {0};
  const char *lista[] = {"one.desktop", "two.desktop",  "hidden.desktop", "bad.desktop",
                         "readme.txt",  "sub.desktop/", ".desktop",       NULL};
  g_dir = a;
  g_names = lista;
  expect(desktop_scan(a, &l) == 2);
  expect(l.n == 2);
  expect(desktop_scan("/nonexistent", &l) == 0);

  const char *listb[] = {"one.desktop", "three.desktop", NULL};
  char bslash[260];
  snprintf(bslash, sizeof bslash, "%s/", b);
  g_dir = bslash;
  g_names = listb;
  expect(desktop_scan(bslash, &l) == 2);
  expect(l.n == 3);

  desktop_sort(&l);
  expect(l.n == 3);
  if (l.n == 3) {
    expect_string(l.v[0]->id, "two");
    expect_string(l.v[1]->id, "three");
    expect_string(l.v[2]->id, "one");
    expect_string(l.v[2]->name, "One Override");
    expect_string(l.v[2]->exec, "one2");
    char want[512];
    snprintf(want, sizeof want, "%s/one.desktop", b);
    expect_string(l.v[2]->location, want);
    snprintf(want, sizeof want, "%s/two.desktop", a);
    expect_string(l.v[0]->location, want);
  }
  desktop_list_free(&l);
  g_dir = NULL;

  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", a);
  expect(system(cmd) == 0);
}

#define APPS_DIR_SUFFIX "/../user/apps"

static void check_shipped(void)
{
  char root[512];
  snprintf(root, sizeof root, "%s", __FILE__);
  char *slash = strrchr(root, '/');
  snprintf(slash ? slash : root, sizeof root - (size_t)(slash ? slash - root : 0), "%s",
           slash ? APPS_DIR_SUFFIX : "../user/apps");
  DIR *apps = opendir(root);
  expect(apps != NULL);
  if (!apps)
    return;
  const char *expect_names[] = {"term",  "files", "archive", "editor",   "browser", "pdfview", "player",
                                "clock", "calc",  "notifyd", "settings", "devhub",  "about"};
  const int nexpect = (int)(sizeof expect_names / sizeof expect_names[0]);
  struct desktop_list l = {0};
  int found = 0;
  for (struct dirent *d; (d = readdir(apps));) {
    if (d->d_name[0] == '.')
      continue;
    char path[1024], file[1100];
    snprintf(path, sizeof path, "%s/%s", root, d->d_name);
    snprintf(file, sizeof file, "%s/%s.desktop", path, d->d_name);
    FILE *f = fopen(file, "rb");
    if (!f)
      continue;
    char text[8192];
    int n = (int)fread(text, 1, sizeof text, f);
    fclose(f);
    found++;
    struct desktop_entry *e = desktop_parse(text, n, d->d_name);
    expect(e != NULL);
    if (!e)
      continue;
    expect(e->name && e->name[0] && e->icon && e->icon[0] && e->categories.n > 0);
    expect(!e->terminal && desktop_visible(e) && e->order != DESKTOP_ORDER_DEFAULT);
    char **argv;
    int argc;
    expect(desktop_exec_argv(e, NULL, NULL, 0, &argv, &argc) == 0);
    if (argc > 0)
      expect_string(argv[0], d->d_name);
    desktop_argv_free(argv, argc);
    l.v = realloc(l.v, (size_t)(l.n + 1) * sizeof *l.v);
    l.v[l.n++] = e;
  }
  closedir(apps);
  expect_eq(found, nexpect);
  desktop_sort(&l);
  for (int i = 0; i < l.n && i < nexpect; i++)
    expect_string(l.v[i]->id, expect_names[i]);
  for (int i = 0; i < l.n; i++) {
    if (!strcmp(l.v[i]->id, "browser")) {
      expect(l.v[i]->mime_types.n == 3 && !strcmp(l.v[i]->mime_types.v[0], "text/html"));
      expect_string(l.v[i]->mime_types.v[2], "x-scheme-handler/https");
    }
  }
  desktop_list_free(&l);
}

int main(void)
{
  test("parse: fields", check_basic);
  test("parse: escapes and lists", check_escapes);
  test("parse: localized keys ignored", check_localized);
  test("parse: groups and actions", check_groups);
  test("parse: malformed input", check_malformed);
  test("parse: rejection", check_validity);
  test("exec: splitting", exec_splitting);
  test("exec: field codes", exec_field_codes);
  test("exec: invalid codes", exec_invalid_codes);
  test("exec: actions", exec_actions);
  test("visible", check_visible);
  test("sort", check_sort);
  test("scan and override", check_scan);
  test("shipped .desktop files", check_shipped);
  return 0;
}
