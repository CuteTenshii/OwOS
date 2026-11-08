/* Host unit test for the browser's DOMTokenList token algorithm. */
#define _POSIX_C_SOURCE 200809L
#define OWOS_SUITE_TEST
#include "classlist.h"
#include <stdlib.h>
#include <string.h>

static void set_class(char **classes, const char *value)
{
  free(*classes);
  *classes = strdup(value ? value : "");
}

static char *classes;

static void check_normalization(void)
{
  classes = strdup("a  b\ta   b\nc");
  classlist_add(&classes, "d", 1);
  expect_string(classes, "a b c d");
  expect(classlist_count(classes) == 4);
  classlist_add(&classes, "b", 1);
  expect_string(classes, "a b c d");
  classlist_remove(&classes, "b", 1);
  expect_string(classes, "a c d");
  classlist_remove(&classes, "d", 1);
  expect_string(classes, "a c");
}

static void check_membership(void)
{
  expect(classlist_has(classes, "a", 1));
  expect(!classlist_has(classes, "a c", 3));
  expect(classlist_has(classes, "c", 1));
  expect(!classlist_has(classes, "x", 1));
}

static void check_toggle(void)
{
  expect(classlist_toggle(&classes, "a") == 0);
  expect_string(classes, "c");
  expect(classlist_toggle(&classes, "a") == 1);
  expect_string(classes, "c a");
}

static void check_replace(void)
{
  set_class(&classes, "one two three");
  expect(classlist_replace(&classes, "two", "TWO"));
  expect_string(classes, "one TWO three");
  expect(!classlist_replace(&classes, "nope", "X"));
  expect_string(classes, "one TWO three");
  expect(classlist_replace(&classes, "one", "three"));
  expect_string(classes, "three TWO");
}

static void check_indexed(void)
{
  int length;
  const char *token;
  char item[32];

  set_class(&classes, "  alpha beta   gamma ");
  token = classlist_nth(classes, 0, &length);
  memcpy(item, token, (size_t)length);
  item[length] = 0;
  expect_string(item, "alpha");
  token = classlist_nth(classes, 2, &length);
  memcpy(item, token, (size_t)length);
  item[length] = 0;
  expect_string(item, "gamma");
  expect(classlist_nth(classes, 3, &length) == 0);
  expect(classlist_count(classes) == 3);
}

static void check_empty(void)
{
  set_class(&classes, "");
  expect(classlist_count(classes) == 0);
  classlist_add(&classes, "first", 5);
  expect_string(classes, "first");
  classlist_remove(&classes, "first", 5);
  expect_string(classes, "");
}

int main(void)
{
  test("normalization and mutation", check_normalization);
  test("whole-token membership", check_membership);
  test("toggle", check_toggle);
  test("replacement and deduplication", check_replace);
  test("indexed access", check_indexed);
  test("empty attribute", check_empty);

  free(classes);
  return 0;
}
