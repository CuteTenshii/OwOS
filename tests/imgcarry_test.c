/* Host unit test for imgcarry.h: inline images keep their landed state across layout walks.
 *
 * Build + run: scripts/run-tests.sh (or `make test`). */
#include <stdio.h>
#define OWOS_SUITE_TEST
#include "imgcarry.h"

static void land(struct inl_img *im, const char *href, int uid, int off, int len)
{
  memset(im, 0, sizeof *im);
  strcpy(im->href, href);
  im->ok = 1;
  im->iw = 10;
  im->ih = 20;
  im->coff = off;
  im->clen = len;
  im->uid = uid;
}

int main(void)
{
  struct inl_img old[4], fresh[4];
  struct img_carry c;

  test("unchanged list keeps uid and pool offsets in order");
  land(&old[0], "http://t/a.png", 7, 0, 100);
  land(&old[1], "http://t/b.png", 8, 100, 50);
  expect(img_carry_begin(&c, old, 2));
  memset(fresh, 0, sizeof fresh);
  strcpy(fresh[0].href, "http://t/a.png");
  strcpy(fresh[1].href, "http://t/b.png");
  expect(img_carry_take(&c, &fresh[0], fresh[0].href, 0, false, fresh));
  expect(img_carry_take(&c, &fresh[1], fresh[1].href, 1, false, fresh));
  expect(fresh[0].uid == 7 && fresh[0].coff == 0 && fresh[0].clen == 100 && fresh[0].ok);
  expect(fresh[1].uid == 8 && fresh[1].coff == 100 && fresh[1].clen == 50 && fresh[1].iw == 10);
  img_carry_end(&c);

  test("reordered images match by href");
  expect(img_carry_begin(&c, old, 2));
  memset(fresh, 0, sizeof fresh);
  strcpy(fresh[0].href, "http://t/b.png");
  expect(img_carry_take(&c, &fresh[0], fresh[0].href, 0, false, fresh));
  expect(fresh[0].uid == 8);
  img_carry_end(&c);

  test("a new href gets nothing and a claimed entry is not reused");
  expect(img_carry_begin(&c, old, 2));
  memset(fresh, 0, sizeof fresh);
  strcpy(fresh[0].href, "http://t/new.png");
  expect(!img_carry_take(&c, &fresh[0], fresh[0].href, 0, false, fresh));
  expect(fresh[0].uid == 0 && !fresh[0].ok);
  strcpy(fresh[1].href, "http://t/a.png");
  strcpy(fresh[2].href, "http://t/a.png");
  expect(img_carry_take(&c, &fresh[1], fresh[1].href, 1, false, fresh));
  expect(!img_carry_take(&c, &fresh[2], fresh[2].href, 2, false, fresh));
  img_carry_end(&c);

  test("need_ok skips entries without pixels and shares built duplicates");
  land(&old[0], "svg:1", 0, 0, 40);
  old[0].ok = 0;
  expect(img_carry_begin(&c, old, 1));
  memset(fresh, 0, sizeof fresh);
  strcpy(fresh[0].href, "svg:1");
  expect(!img_carry_take(&c, &fresh[0], fresh[0].href, 0, true, fresh));
  img_carry_end(&c);
  land(&fresh[0], "svg:2", 0, 64, 16);
  strcpy(fresh[1].href, "svg:2");
  expect(img_carry_begin(&c, old, 0) == false);
  expect(img_carry_take(&c, &fresh[1], fresh[1].href, 1, true, fresh));
  expect(fresh[1].coff == 64 && fresh[1].clen == 16);
  img_carry_end(&c);

  test("lazy fetch progress survives");
  land(&old[0], "http://t/lazy.png", 0, 0, 0);
  old[0].ok = 0;
  old[0].fetched = 1;
  old[0].want = 1;
  expect(img_carry_begin(&c, old, 1));
  memset(fresh, 0, sizeof fresh);
  strcpy(fresh[0].href, "http://t/lazy.png");
  expect(img_carry_take(&c, &fresh[0], fresh[0].href, 0, false, fresh));
  expect(fresh[0].fetched == 1 && fresh[0].want == 1 && !fresh[0].ok);
  img_carry_end(&c);

  test("pool is kept only while an entry points into it");
  land(&old[0], "a", 1, 0, 30);
  expect(img_carry_pool_len(old, 1, 500) == 500);
  img_clear(&old[0]);
  expect(img_carry_pool_len(old, 1, 500) == 0);
  expect(img_carry_pool_len(old, 0, 500) == 0);

  test("img_clear resets landed state");
  land(&old[0], "a", 3, 8, 9);
  old[0].fetched = 1;
  old[0].want = 1;
  img_clear(&old[0]);
  expect(!old[0].ok && !old[0].uid && !old[0].clen && !old[0].fetched && !old[0].want && !old[0].iw);
  return 0;
}
