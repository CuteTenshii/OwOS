/* owofs_migrate_test.c: host test for the owofs v8 -> v9 conversion in kernel/fsmigrate.h. */
#define OWOS_SUITE_TEST
#include "../kernel/fsmigrate.h"
#include <stdlib.h>
#include <string.h>

#define SLOTS 32

static struct owofs_dirent_v8 old[SLOTS];
static struct owofs_dirent out[SLOTS];

static void put(struct owofs_dirent_v8 *d, int slot, const char *name, int dir, int parent, unsigned size,
                unsigned start)
{
  memset(&d[slot], 0, sizeof d[slot]);
  strcpy(d[slot].name, name);
  d[slot].flags = dir ? FSM_DIR_FLAG : 0;
  d[slot].parent = (uint16_t)parent;
  d[slot].size = size;
  d[slot].start = start;
  d[slot].mtime = 0x1234u + (unsigned)slot;
}

static void convert(const struct owofs_dirent_v8 *old, struct owofs_dirent *out)
{
  fsm_convert(old, SLOTS, out, SLOTS);
}

static void test_entry_sizes(void)
{
  expect(sizeof(struct owofs_dirent_v8) == 80);
  expect(sizeof(struct owofs_dirent) == 88);
}

static void test_empty_directory_converts_to(void)
{
  memset(old, 0, sizeof old);
  memset(out, 0xAA, sizeof out);
  convert(old, out);
  int zero = 1;
  for (size_t i = 0; i < sizeof out; i++)
    zero &= ((uint8_t *)out)[i] == 0;
  expect(zero);
}

static void test_defaults_dirs_0755_files(void)
{
  memset(old, 0, sizeof old);
  put(old, 0, "bin", 1, FSM_ROOT, 0, 0);
  put(old, 1, "ls", 0, 0, 100, 10);
  put(old, 2, "etc", 1, FSM_ROOT, 0, 0);
  put(old, 3, "passwd", 0, 2, 21, 20);
  put(old, 4, "motd", 0, 2, 5, 30);
  put(old, 5, "readme", 0, FSM_ROOT, 7, 40);
  put(old, 6, "sub", 1, 0, 0, 0);
  put(old, 7, "tool", 0, 6, 9, 50);
  put(old, 8, "home", 1, FSM_ROOT, 0, 0);
  put(old, 9, "passwd", 0, 8, 3, 60);
  put(old, 10, "passwd", 0, FSM_ROOT, 3, 70);
  convert(old, out);
  expect(out[0].mode == 0755);
  expect(out[1].mode == 0755);
  expect(out[2].mode == 0755);
  expect(out[3].mode == 0600);
  expect(out[4].mode == 0644);
  expect(out[5].mode == 0644);
  expect(out[6].mode == 0755);
  expect(out[7].mode == 0755);
  expect(out[8].mode == 0755);
  expect(out[9].mode == 0644);
  expect(out[10].mode == 0644);
  int owners = 1;
  for (int i = 0; i < SLOTS; i++)
    owners &= out[i].uid == 0 && out[i].gid == 0;
  expect(owners);
}

static void test_a_file_named_bin(void)
{
  memset(old, 0, sizeof old);
  put(old, 0, "bin", 0, FSM_ROOT, 4, 10);
  convert(old, out);
  expect(out[0].mode == 0644);
}

static void test_every_field_survives_and(void)
{
  memset(old, 0, sizeof old);
  put(old, 3, "alpha", 0, FSM_ROOT, 4096, 77);
  put(old, 9, "dir", 1, FSM_ROOT, 0, 0);
  put(old, 20, "leaf", 0, 9, 1, 5);
  convert(old, out);
  int ok = 1;
  for (int i = 0; i < SLOTS; i++) {
    if (!old[i].name[0]) {
      ok &= out[i].name[0] == 0 && out[i].mode == 0 && out[i].size == 0;
      continue;
    }
    ok &= memcmp(out[i].name, old[i].name, FSM_NAME_LEN) == 0;
    ok &= out[i].flags == old[i].flags && out[i].parent == old[i].parent;
    ok &= out[i].size == old[i].size && out[i].start == old[i].start && out[i].mtime == old[i].mtime;
    ok &= out[i].mode != 0;
  }
  expect(ok);
}

static void test_full_directory_with_deep(void)
{
  memset(old, 0, sizeof old);
  put(old, 0, "bin", 1, FSM_ROOT, 0, 0);
  for (int i = 1; i < SLOTS - 1; i++)
    put(old, i, "d", 1, i - 1, 0, 0);
  put(old, SLOTS - 1, "deep", 0, SLOTS - 2, 11, 99);
  convert(old, out);
  expect(out[SLOTS - 1].mode == 0755);
  expect(out[SLOTS - 1].size == 11);
  strcpy(old[0].name, "home");
  convert(old, out);
  expect(out[SLOTS - 1].mode == 0644);
  expect(out[SLOTS - 2].mode == 0755);
}

static void test_broken_parent_chains_fall(void)
{
  memset(old, 0, sizeof old);
  put(old, 0, "a", 0, 1, 1, 1);
  put(old, 1, "b", 0, 0, 1, 1);
  put(old, 2, "c", 0, 500, 1, 1);
  put(old, 3, "d", 0, 4, 1, 1);
  convert(old, out);
  expect(out[0].mode == 0644 && out[1].mode == 0644 && out[2].mode == 0644 && out[3].mode == 0644);
}

static void test_new_extent_always_holds(void)
{
  int fits = 1;
  for (uint32_t s = 1; s < 4000; s++) {
    uint32_t n = fsm_v9_sectors(s);
    fits &= fsm_entries_v9(n) >= fsm_entries_v8(s);
    fits &= n >= s;
    if (n > 1)
      fits &= fsm_entries_v9(n - 1) < fsm_entries_v8(s);
  }
  expect(fits);
}

static void test_randomised_layouts_round_trip(void)
{
  srand(7);
  int rt = 1;
  for (int round = 0; round < 200; round++) {
    memset(old, 0, sizeof old);
    for (int i = 0; i < SLOTS; i++) {
      if (rand() % 3 == 0)
        continue;
      char nm[8];
      nm[0] = (char)('a' + rand() % 26);
      nm[1] = (char)('a' + rand() % 26);
      nm[2] = 0;
      put(old, i, nm, rand() % 2, i == 0 || rand() % 2 ? FSM_ROOT : rand() % i, (unsigned)rand(), (unsigned)rand());
    }
    convert(old, out);
    for (int i = 0; i < SLOTS; i++) {
      if (!old[i].name[0]) {
        rt &= out[i].name[0] == 0;
        continue;
      }
      rt &= strcmp(out[i].name, old[i].name) == 0 && out[i].flags == old[i].flags;
      rt &= out[i].parent == old[i].parent && out[i].size == old[i].size;
      rt &= out[i].start == old[i].start && out[i].mtime == old[i].mtime;
      rt &= (out[i].flags & FSM_DIR_FLAG) ? out[i].mode == 0755 : (out[i].mode & 0644) == 0644;
    }
  }
  expect(rt);
}

int main(void)
{
  test("entry sizes", test_entry_sizes);
  test("empty directory converts to an empty directory", test_empty_directory_converts_to);
  test("defaults: dirs 0755, files 0644, /bin files 0755, /etc/passwd 0600, root:root", test_defaults_dirs_0755_files);
  test("a file named bin at the root is not a /bin directory", test_a_file_named_bin);
  test("every field survives and empty slots stay empty", test_every_field_survives_and);
  test("full directory with deep nesting follows the top-level ancestor", test_full_directory_with_deep);
  test("broken parent chains fall back to 0644 without looping", test_broken_parent_chains_fall);
  test("new extent always holds every slot of the old one", test_new_extent_always_holds);
  test("randomised layouts round-trip", test_randomised_layouts_round_trip);

  return 0;
}
