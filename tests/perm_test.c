/* perm_test.c: host test for kernel/perm.h and user/lib/modeutil.h (chmod parsing). */
#define OWOS_SUITE_TEST
#include "../kernel/perm.h"
#include "modeutil.h"
#include <string.h>

static unsigned parsed(const char *spec, unsigned cur, unsigned um, int dir)
{
  unsigned out = 0xFFFFu;
  return mode_parse(spec, cur, um, dir, &out) ? out : 0xFFFFu;
}

static void check_class(void)
{
  expect(perm_class(0640, 10, 20, 10, 99) == 6);
  expect(perm_class(0640, 10, 20, 11, 20) == 4);
  expect(perm_class(0640, 10, 20, 11, 21) == 0);
}

static void check_owner_wins(void)
{
  expect(!perm_allows(0077, 10, 20, 10, 20, PERM_R, false));
  expect(perm_allows(0077, 10, 20, 11, 20, PERM_R | PERM_W | PERM_X, false));
  expect(!perm_allows(0707, 10, 20, 11, 20, PERM_R, false));
}

static void check_want_covered(void)
{
  expect(perm_allows(0644, 10, 20, 11, 99, PERM_R, false));
  expect(!perm_allows(0644, 10, 20, 11, 99, PERM_R | PERM_W, false));
  expect(perm_allows(0644, 10, 20, 10, 20, PERM_R | PERM_W, false));
  expect(perm_allows(0600, 10, 20, 10, 20, 0, false));
}

static void check_root(void)
{
  expect(perm_allows(0000, 10, 20, 0, 0, PERM_R | PERM_W, false));
  expect(!perm_allows(0644, 10, 20, 0, 0, PERM_X, false));
  expect(perm_allows(0641, 10, 20, 0, 0, PERM_X, false));
  expect(perm_allows(0010, 10, 20, 0, 0, PERM_X, false));
  expect(perm_allows(0000, 10, 20, 0, 0, PERM_X, true));
}

static void check_traversal(void)
{
  expect(perm_allows(0711, 10, 20, 11, 21, PERM_X, true));
  expect(!perm_allows(0700, 10, 20, 11, 21, PERM_X, true));
  expect(!perm_allows(0644, 10, 20, 11, 21, PERM_X, true));
  expect(perm_allows(0750, 10, 20, 11, 20, PERM_X, true));
}

static void check_create_delete(void)
{
  expect(perm_allows(0755, 10, 20, 10, 20, PERM_W | PERM_X, true));
  expect(!perm_allows(0755, 10, 20, 11, 21, PERM_W | PERM_X, true));
  expect(perm_allows(0777, 10, 20, 11, 21, PERM_W | PERM_X, true));
  expect(!perm_allows(0766, 10, 20, 11, 21, PERM_W | PERM_X, true));
}

static void check_sticky(void)
{
  expect(perm_sticky_allows(0777, 1, 2, 3));
  expect(perm_sticky_allows(01777, 1, 2, 2));
  expect(perm_sticky_allows(01777, 1, 2, 1));
  expect(perm_sticky_allows(01777, 1, 2, 0));
  expect(!perm_sticky_allows(01777, 1, 2, 3));
}

static void check_umask(void)
{
  expect(perm_apply_umask(0666, 022) == 0644);
  expect(perm_apply_umask(0777, 022) == 0755);
  expect(perm_apply_umask(0666, 077) == 0600);
  expect(perm_apply_umask(0777, 0) == 0777);
  expect(perm_apply_umask(0666, 0777) == 0);
  expect(perm_apply_umask(0666, 0002) == 0664);
}

static void check_chmod_chown(void)
{
  expect(perm_may_chmod(5, 5));
  expect(perm_may_chmod(5, 0));
  expect(!perm_may_chmod(5, 6));
  expect(perm_may_chown(5, 0, 0, 7, 8));
  expect(!perm_may_chown(5, 5, 50, 7, 50));
  expect(perm_may_chown(5, 5, 50, 5, 50));
  expect(!perm_may_chown(5, 5, 50, 5, 51));
  expect(!perm_may_chown(5, 6, 50, 5, 50));
}

static void check_mode_str(void)
{
  char s[11];
  perm_mode_str(0755, true, s);
  expect_string(s, "drwxr-xr-x");
  perm_mode_str(0644, false, s);
  expect_string(s, "-rw-r--r--");
  perm_mode_str(01777, true, s);
  expect_string(s, "drwxrwxrwt");
  perm_mode_str(01776, true, s);
  expect_string(s, "drwxrwxrwT");
  perm_mode_str(0, false, s);
  expect_string(s, "----------");
}

static void check_chmod_octal(void)
{
  expect(parsed("755", 0, 0, 0) == 0755);
  expect(parsed("0644", 0777, 0, 0) == 0644);
  expect(parsed("1777", 0, 0, 1) == 01777);
  expect(parsed("0", 0777, 0, 0) == 0);
  expect(parsed("888", 0, 0, 0) == 0xFFFFu);
  expect(parsed("4755", 0, 0, 0) == 0xFFFFu);
  expect(parsed("77777", 0, 0, 0) == 0xFFFFu);
  expect(parsed("", 0, 0, 0) == 0xFFFFu);
}

static void check_chmod_symbolic(void)
{
  expect(parsed("u+x", 0644, 0, 0) == 0744);
  expect(parsed("go-w", 0666, 0, 0) == 0644);
  expect(parsed("a=r", 0777, 0, 0) == 0444);
  expect(parsed("o=", 0777, 0, 0) == 0770);
  expect(parsed("u=rwx,g=rx,o=", 0, 0, 0) == 0750);
  expect(parsed("g+r,o+r", 0600, 0, 0) == 0644);
  expect(parsed("u+x-w", 0644, 0, 0) == 0544);
  expect(parsed("g=u", 0750, 0, 0) == 0770);
  expect(parsed("o+t", 0777, 0, 1) == 01777);
  expect(parsed("+t", 0777, 0, 1) == 01777);
  expect(parsed("-t", 01777, 0, 1) == 0777);
}

static void check_chmod_symbolic_who(void)
{
  expect(parsed("+x", 0644, 0, 0) == 0755);
  expect(parsed("+w", 0444, 022, 0) == 0644);
  expect(parsed("=rw", 0777, 022, 0) == 0644);
  expect(parsed("a+w", 0444, 022, 0) == 0666);
}

static void check_chmod_symbolic_x(void)
{
  expect(parsed("a+X", 0644, 0, 0) == 0644);
  expect(parsed("a+X", 0744, 0, 0) == 0755);
  expect(parsed("a+X", 0600, 0, 1) == 0711);
}

static void check_chmod_symbolic_bad(void)
{
  expect(parsed("u+z", 0644, 0, 0) == 0xFFFFu);
  expect(parsed("u+s", 0644, 0, 0) == 0xFFFFu);
  expect(parsed("x", 0644, 0, 0) == 0xFFFFu);
  expect(parsed("u+x,", 0644, 0, 0) == 0xFFFFu);
  expect(parsed("u", 0644, 0, 0) == 0xFFFFu);
}

int main(void)
{
  test("class selection: owner, group, other", check_class);
  test("owner bits win even when group or other are wider", check_owner_wins);
  test("want must be fully covered", check_want_covered);
  test("root bypasses rw but a file still needs an x bit to exec", check_root);
  test("directory traversal needs x on the matching class", check_traversal);
  test("create or delete needs w+x on the parent", check_create_delete);
  test("sticky directories restrict removal to the entry owner, directory owner or root", check_sticky);
  test("umask math", check_umask);
  test("chmod and chown authority", check_chmod_chown);
  test("mode string", check_mode_str);
  test("chmod octal", check_chmod_octal);
  test("chmod symbolic", check_chmod_symbolic);
  test("chmod symbolic: who defaults to all, limited by umask", check_chmod_symbolic_who);
  test("chmod symbolic: X only sets x for directories or already-executable files", check_chmod_symbolic_x);
  test("chmod symbolic: rejects bad input", check_chmod_symbolic_bad);
  return 0;
}
