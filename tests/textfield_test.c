/* Host test for the single-line text field (user/lib/gfx/textfield.c) with the gfx primitives stubbed. */
#define OWOS_SUITE_TEST
#include "gfx.h"
#include "owos.h"
#include "test.h"
#include <string.h>

#define CW 8

static int highlighted;
int gfx_ui_cw(unsigned char c)
{
  (void)c;
  return CW;
}
void gfx_fill_rect(int x, int y, int w, int h, unsigned color)
{
  (void)x;
  (void)y;
  (void)h;
  if (w == CW && color == 0x2f6bd8u)
    highlighted++;
}
void gfx_rect(int x, int y, int w, int h, unsigned color)
{
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)color;
}
void gfx_ui_text(int x, int y, const char *s, unsigned fg)
{
  (void)x;
  (void)y;
  (void)s;
  (void)fg;
}

static char buf[16];
static struct textfield tf;

static void fresh(const char *text)
{
  textfield_init(&tf, buf, sizeof buf);
  textfield_set(&tf, text);
}

static void check_select_all(void)
{
  fresh("hello");
  expect(textfield_key(&tf, 'a', KMOD_CTRL));
  int a, b;
  expect(textfield_selection(&tf, &a, &b));
  expect_eq(a, 0);
  expect_eq(b, 5);
  expect_string(buf, "hello");
}

static void check_type_replaces(void)
{
  fresh("hello");
  textfield_key(&tf, 'a', KMOD_CTRL);
  textfield_key(&tf, 'x', 0);
  expect_string(buf, "x");
  expect_eq(tf.caret, 1);
  expect_eq(tf.anchor, 1);
}

static void check_delete_selection(void)
{
  fresh("hello");
  textfield_key(&tf, KEY_LEFT, 0);
  textfield_key(&tf, KEY_LEFT, KMOD_SHIFT);
  textfield_key(&tf, KEY_LEFT, KMOD_SHIFT);
  textfield_key(&tf, '\b', 0);
  expect_string(buf, "heo");
  expect_eq(tf.caret, 2);
  fresh("hello");
  textfield_key(&tf, KEY_HOME, 0);
  textfield_key(&tf, KEY_RIGHT, KMOD_SHIFT);
  textfield_key(&tf, KEY_DELETE, 0);
  expect_string(buf, "ello");
  expect_eq(tf.len, 4);
}

static void check_shift_arrows(void)
{
  fresh("hello");
  textfield_key(&tf, KEY_END, KMOD_SHIFT);
  int a, b;
  expect(!textfield_selection(&tf, &a, &b));
  textfield_key(&tf, KEY_HOME, KMOD_SHIFT);
  expect(textfield_selection(&tf, &a, &b));
  expect_eq(a, 0);
  expect_eq(b, 5);
  textfield_key(&tf, KEY_LEFT, 0);
  expect_eq(tf.caret, 0);
  expect(!textfield_selection(&tf, &a, &b));
  textfield_key(&tf, KEY_END, 0);
  textfield_key(&tf, KEY_LEFT, KMOD_SHIFT);
  textfield_key(&tf, KEY_RIGHT, 0);
  expect_eq(tf.caret, 5);
}

static void check_ctrl_letters_not_inserted(void)
{
  fresh("ab");
  expect(!textfield_key(&tf, 'q', KMOD_CTRL));
  expect_string(buf, "ab");
}

static void check_full_buffer(void)
{
  fresh("0123456789abcde");
  expect(textfield_key(&tf, 'x', 0));
  expect_string(buf, "0123456789abcde");
  textfield_key(&tf, 'a', KMOD_CTRL);
  textfield_key(&tf, 'x', 0);
  expect_string(buf, "x");
}

static void check_mouse(void)
{
  fresh("hello");
  expect(textfield_click(&tf, 4 + 2 * CW + 1, 5, 0, 0, 100, 20));
  expect_eq(tf.caret, 2);
  expect_eq(tf.anchor, 2);
  textfield_drag(&tf, 4 + 4 * CW + 1, 0, 100);
  expect_eq(tf.caret, 4);
  expect_eq(tf.anchor, 2);
  textfield_drag(&tf, 500, 0, 100);
  expect_eq(tf.caret, 5);
  textfield_drag(&tf, -5, 0, 100);
  expect_eq(tf.caret, 0);
  expect(!textfield_click(&tf, 200, 5, 0, 0, 100, 20));
}

static void check_draw_highlight(void)
{
  fresh("hello");
  tf.anchor = 1;
  tf.caret = 4;
  highlighted = 0;
  textfield_draw(&tf, 0, 0, 100, 20, 1);
  expect_eq(highlighted, 3);
}

static void type(const char *s)
{
  for (; *s; s++)
    textfield_key(&tf, *s, 0);
}

static void check_undo_redo(void)
{
  fresh("");
  type("abc");
  textfield_key(&tf, KEY_LEFT, 0);
  type("de");
  expect_string(buf, "abdec");
  textfield_key(&tf, 'z', KMOD_CTRL);
  expect_string(buf, "abc");
  expect_eq(tf.caret, 2);
  textfield_key(&tf, 'z', KMOD_CTRL);
  expect_string(buf, "");
  textfield_key(&tf, 'z', KMOD_CTRL);
  expect_string(buf, "");
  textfield_key(&tf, 'y', KMOD_CTRL);
  expect_string(buf, "abc");
  textfield_key(&tf, 'z', KMOD_CTRL | KMOD_SHIFT);
  expect_string(buf, "abdec");
  textfield_key(&tf, 'y', KMOD_CTRL);
  expect_string(buf, "abdec");
}

static void check_undo_deletes(void)
{
  fresh("hello");
  textfield_key(&tf, '\b', 0);
  textfield_key(&tf, '\b', 0);
  expect_string(buf, "hel");
  textfield_key(&tf, 'z', KMOD_CTRL);
  expect_string(buf, "hell");
  textfield_key(&tf, 'z', KMOD_CTRL);
  expect_string(buf, "hello");
  textfield_key(&tf, 'a', KMOD_CTRL);
  type("x");
  textfield_key(&tf, 'z', KMOD_CTRL);
  expect_string(buf, "hello");
}

static void check_edit_clears_redo(void)
{
  fresh("");
  type("ab");
  textfield_key(&tf, 'z', KMOD_CTRL);
  type("c");
  textfield_key(&tf, 'y', KMOD_CTRL);
  expect_string(buf, "c");
  expect_eq(tf.nredo, 0);
}

static void check_set_clears_history(void)
{
  fresh("");
  type("ab");
  textfield_set(&tf, "new");
  textfield_key(&tf, 'z', KMOD_CTRL);
  expect_string(buf, "new");
}

static void check_insert(void)
{
  fresh("hello");
  textfield_key(&tf, KEY_LEFT, KMOD_SHIFT);
  textfield_key(&tf, KEY_LEFT, KMOD_SHIFT);
  textfield_insert(&tf, "a\nb\x01~", 5);
  expect_string(buf, "hela b~");
  expect_eq(tf.caret, 7);
  textfield_key(&tf, 'z', KMOD_CTRL);
  expect_string(buf, "hello");
  fresh("0123456789abcd");
  textfield_insert(&tf, "xyz", 3);
  expect_string(buf, "0123456789abcdx");
}

int main(void)
{
  test("ctrl+a selects everything", check_select_all);
  test("typing replaces the selection", check_type_replaces);
  test("backspace and delete remove the selection", check_delete_selection);
  test("shift+motion extends, plain motion collapses", check_shift_arrows);
  test("ctrl+letter is not inserted", check_ctrl_letters_not_inserted);
  test("full buffer", check_full_buffer);
  test("click and drag", check_mouse);
  test("selection is highlighted", check_draw_highlight);
  test("insert replaces the selection and filters", check_insert);
  test("undo and redo", check_undo_redo);
  test("undo of deletes and replaced selections", check_undo_deletes);
  test("an edit discards redo", check_edit_clears_redo);
  test("textfield_set clears history", check_set_clears_history);
  return 0;
}
