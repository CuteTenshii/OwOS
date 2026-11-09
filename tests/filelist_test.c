/* Host test for the file picker's directory model (user/lib/gfx/filelist.c) over a fake readdir. */
#define OWOS_SUITE_TEST
#include "filelist_fs.h"
#include "gfx.h"
#include "owos.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

struct fake {
  const char *name;
  int is_dir;
  unsigned size;
};

static const struct fake *g_dir;
static int g_count;

int filelist_test_readdir(const char *path, int idx, struct dirent_u *out)
{
  if (strcmp(path, "/music") != 0 || idx >= g_count)
    return 0;
  strlcpy(out->name, g_dir[idx].name, sizeof out->name);
  out->is_dir = g_dir[idx].is_dir;
  out->size = g_dir[idx].size;
  return 1;
}

static const struct fake k_music[] = {
    {"zebra.mp3", 0, 10}, {"Readme.TXT", 0, 20}, {"album", 1, 0},     {"alpha.WAV", 0, 30},
    {"Beta", 1, 0},       {"cover.png", 0, 40},  {"song.mp3", 0, 50},
};
static const char *const k_audio[] = {".mp3", ".wav", NULL};

static void use(const struct fake *d, int n)
{
  g_dir = d;
  g_count = n;
}

static void check_sorted_dirs_first(void)
{
  use(k_music, 7);
  struct filelist_entry *v;
  expect_eq(filelist_read("/music", NULL, 0, &v), 7);
  const char *want[] = {"album", "Beta", "alpha.WAV", "cover.png", "Readme.TXT", "song.mp3", "zebra.mp3"};
  for (int i = 0; i < 7; i++)
    expect_string(v[i].name, want[i]);
  expect(v[0].is_dir && v[1].is_dir && !v[2].is_dir);
  expect_eq(v[6].size, 10);
  free(v);
}

static void check_extension_filter(void)
{
  use(k_music, 7);
  struct filelist_entry *v;
  expect_eq(filelist_read("/music", k_audio, 0, &v), 5);
  const char *want[] = {"album", "Beta", "alpha.WAV", "song.mp3", "zebra.mp3"};
  for (int i = 0; i < 5; i++)
    expect_string(v[i].name, want[i]);
  free(v);
}

static void check_dirs_only(void)
{
  use(k_music, 7);
  struct filelist_entry *v;
  expect_eq(filelist_read("/music", k_audio, 1, &v), 2);
  expect(v[0].is_dir && v[1].is_dir);
  free(v);
}

static void check_missing_dir_is_empty(void)
{
  use(k_music, 7);
  struct filelist_entry *v;
  expect_eq(filelist_read("/nowhere", NULL, 0, &v), 0);
  free(v);
}

static void check_grows_past_initial_capacity(void)
{
  static struct fake many[200];
  static char names[200][16];
  for (int i = 0; i < 200; i++) {
    snprintf(names[i], sizeof names[i], "f%03d", 199 - i);
    many[i] = (struct fake){names[i], 0, 0};
  }
  use(many, 200);
  struct filelist_entry *v;
  expect_eq(filelist_read("/music", NULL, 0, &v), 200);
  expect_string(v[0].name, "f000");
  expect_string(v[199].name, "f199");
  free(v);
}

static void check_match(void)
{
  expect(filelist_match("a.MP3", k_audio));
  expect(!filelist_match("mp3", k_audio));
  expect(!filelist_match("a.mp3.txt", k_audio));
  expect(filelist_match("anything", NULL));
}

static void check_join(void)
{
  char p[64];
  filelist_join(p, sizeof p, "/", "a");
  expect_string(p, "/a");
  filelist_join(p, sizeof p, "/music", "a.mp3");
  expect_string(p, "/music/a.mp3");
  filelist_join(p, sizeof p, "/music", "/etc/x");
  expect_string(p, "/etc/x");
}

static void check_parent(void)
{
  char p[64];
  strcpy(p, "/a/b");
  expect(filelist_parent(p));
  expect_string(p, "/a");
  expect(filelist_parent(p));
  expect_string(p, "/");
  expect(!filelist_parent(p));
  expect_string(p, "/");
  strcpy(p, "/a/b//");
  expect(filelist_parent(p));
  expect_string(p, "/a");
}

int main(void)
{
  test("sorted, directories first, case-insensitive", check_sorted_dirs_first);
  test("extension filter keeps directories", check_extension_filter);
  test("dirs_only drops files", check_dirs_only);
  test("missing directory lists nothing", check_missing_dir_is_empty);
  test("grows past the initial capacity", check_grows_past_initial_capacity);
  test("extension match is a case-insensitive suffix", check_match);
  test("join", check_join);
  test("parent", check_parent);
  return 0;
}
