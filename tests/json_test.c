/* Host test for the ownership rules of user/lib/json's object builders. */
#define OWOS_SUITE_TEST
#include "json.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static void check_put_inserts_and_replaces(void)
{
  json_value *o = json_new_object();
  expect(json_object_put(o, "a", json_new_number(1)) == 0);
  expect(json_object_put(o, "a", json_new_string("two")) == 0);
  expect_string(json_as_string(json_object_get(o, "a")), "two");
  char *enc = json_encode(o, 0, 0);
  expect_string(enc, "{\"a\":\"two\"}");
  free(enc);
  json_free(o);
}

static void check_put_consumes_on_failure(void)
{
  json_value *arr = json_new_array(), *obj = json_new_object();
  /* Each value is freed by put; a leak or double free shows up under valgrind or ASan */
  expect(json_object_put(0, "k", json_new_string("x")) == -1);
  expect(json_object_put(arr, "k", json_new_string("x")) == -1);
  expect(json_object_put(obj, 0, json_new_number(3)) == -1);
  expect(json_object_put(obj, "k", 0) == -1);
  json_free(arr);
  json_free(obj);
}

static void check_set_leaves_ownership_on_failure(void)
{
  json_value *v = json_new_string("kept");
  expect(json_object_set(0, "k", v) == -1);
  expect_string(json_as_string(v), "kept");
  json_free(v);
}

int main(void)
{
  test("put inserts and replaces", check_put_inserts_and_replaces);
  test("put frees the value when it fails", check_put_consumes_on_failure);
  test("set leaves the value with the caller when it fails", check_set_leaves_ownership_on_failure);
  return 0;
}
