/* Host unit test for the browser classList (DOMTokenList) token algorithm.
 *
 *   cc -std=c11 scripts/classlist_test.c -o /tmp/classlist_test && /tmp/classlist_test
 *
 * Mirrors the pure string helpers from user/apps/gui/browser/js.c verbatim
 * (sbuf + cl_is_ws/cl_has/cl_count/cl_nth/cl_emit and the write ops), driving a
 * plain char* "class attribute" instead of a live DOM node. Asserts the
 * normalized serialization of add/remove/toggle/replace/contains/count/item. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *b; int n, cap; } sbuf;
static void sb_need(sbuf *s, int extra) {
    if (s->n + extra + 1 > s->cap) {
        int c = s->cap ? s->cap * 2 : 128;
        while (c < s->n + extra + 1) c *= 2;
        s->b = (char *)realloc(s->b, (unsigned)c);
        s->cap = c;
    }
}
static void sb_putc(sbuf *s, char c) { sb_need(s, 1); s->b[s->n++] = c; s->b[s->n] = 0; }
static void sb_putn(sbuf *s, const char *t, int n) { sb_need(s, n); memcpy(s->b + s->n, t, (unsigned)n); s->n += n; s->b[s->n] = 0; }
static int memcmp_js(const char *a, const char *b, int n) {
    int i; for (i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0;
}

static int cl_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int cl_has(const char *s, const char *tok, int tl) {
    if (!s || tl <= 0) return 0;
    while (*s) {
        const char *b;
        while (cl_is_ws(*s)) s++;
        b = s;
        while (*s && !cl_is_ws(*s)) s++;
        if (s - b == tl && memcmp_js(b, tok, tl) == 0) return 1;
    }
    return 0;
}
static int cl_count(const char *s) {
    int n = 0;
    while (*s) {
        while (cl_is_ws(*s)) s++;
        if (!*s) break;
        n++;
        while (*s && !cl_is_ws(*s)) s++;
    }
    return n;
}
static const char *cl_nth(const char *s, int idx, int *len) {
    int n = 0;
    while (*s) {
        const char *b;
        while (cl_is_ws(*s)) s++;
        if (!*s) break;
        b = s;
        while (*s && !cl_is_ws(*s)) s++;
        if (n == idx) { *len = (int)(s - b); return b; }
        n++;
    }
    return 0;
}
static void cl_emit(sbuf *sb, const char *tok, int tl) {
    if (tl <= 0) return;
    if (sb->b && cl_has(sb->b, tok, tl)) return;
    if (sb->n) sb_putc(sb, ' ');
    sb_putn(sb, tok, tl);
}

/* the write ops operate on a char** (the "class attribute") instead of a node */
static void set_class(char **cls, const char *v) { free(*cls); *cls = strdup(v ? v : ""); }

static void cl_write_add(char **cls, const char *tok, int tl) {
    const char *cur = *cls; const char *b; int len, i;
    sbuf sb; sb.b = 0; sb.n = 0; sb.cap = 0;
    if (!cur) cur = "";
    for (i = 0; (b = cl_nth(cur, i, &len)); i++) cl_emit(&sb, b, len);
    cl_emit(&sb, tok, tl);
    set_class(cls, sb.b ? sb.b : "");
    if (sb.b) free(sb.b);
}
static void cl_write_remove(char **cls, const char *tok, int tl) {
    const char *cur = *cls; const char *b; int len, i;
    sbuf sb; sb.b = 0; sb.n = 0; sb.cap = 0;
    if (!cur) cur = "";
    for (i = 0; (b = cl_nth(cur, i, &len)); i++)
        if (!(len == tl && memcmp_js(b, tok, len) == 0)) cl_emit(&sb, b, len);
    set_class(cls, sb.b ? sb.b : "");
    if (sb.b) free(sb.b);
}
static int cl_replace(char **cls, const char *oldt, const char *newt) {
    int ol = (int)strlen(oldt), nl = (int)strlen(newt);
    const char *cur = *cls, *b; int len, i;
    sbuf sb;
    if (ol == 0 || nl == 0) return 0;
    if (!cl_has(cur, oldt, ol)) return 0;
    sb.b = 0; sb.n = 0; sb.cap = 0;
    for (i = 0; (b = cl_nth(cur, i, &len)); i++) {
        if (len == ol && memcmp_js(b, oldt, len) == 0) cl_emit(&sb, newt, nl);
        else cl_emit(&sb, b, len);
    }
    set_class(cls, sb.b ? sb.b : "");
    if (sb.b) free(sb.b);
    return 1;
}
static int cl_toggle(char **cls, const char *tok) {
    int tl = (int)strlen(tok);
    if (tl == 0) return 0;
    if (cl_has(*cls, tok, tl)) { cl_write_remove(cls, tok, tl); return 0; }
    cl_write_add(cls, tok, tl); return 1;
}

static int fails;
static void check_str(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) { printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got, want); fails++; }
    else printf("ok   %s = \"%s\"\n", what, got);
}
static void check_int(const char *what, int got, int want) {
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); fails++; }
    else printf("ok   %s = %d\n", what, got);
}

int main(void) {
    char *cls;

    /* normalization: dupes + irregular whitespace collapse on first mutation */
    cls = strdup("a  b\ta   b\nc");
    cl_write_add(&cls, "d", 1);
    check_str("add d (normalize)", cls, "a b c d");
    check_int("count", cl_count(cls), 4);

    /* add: existing token is a no-op */
    cl_write_add(&cls, "b", 1);
    check_str("add existing b", cls, "a b c d");

    /* remove middle + trailing */
    cl_write_remove(&cls, "b", 1);
    check_str("remove b", cls, "a c d");
    cl_write_remove(&cls, "d", 1);
    check_str("remove d", cls, "a c");

    /* contains, whole-token only (no substring match) */
    check_int("contains a", cl_has(cls, "a", 1), 1);
    check_int("contains 'a c' (not a token)", cl_has(cls, "a c", 3), 0);
    check_int("contains substring 'c'... yes", cl_has(cls, "c", 1), 1);
    check_int("contains x", cl_has(cls, "x", 1), 0);

    /* toggle off then on */
    check_int("toggle a -> off", cl_toggle(&cls, "a"), 0);
    check_str("after toggle a", cls, "c");
    check_int("toggle a -> on", cl_toggle(&cls, "a"), 1);
    check_str("after toggle a on", cls, "c a");

    /* replace present token; dedupes if target already present */
    set_class(&cls, "one two three");
    check_int("replace two->TWO", cl_replace(&cls, "two", "TWO"), 1);
    check_str("after replace", cls, "one TWO three");
    check_int("replace missing", cl_replace(&cls, "nope", "X"), 0);
    check_str("after failed replace", cls, "one TWO three");
    check_int("replace one->three (dedupe)", cl_replace(&cls, "one", "three"), 1);
    check_str("after dedupe replace", cls, "three TWO");

    /* item()/nth indexing */
    set_class(&cls, "  alpha beta   gamma ");
    { int len; const char *b; char tmp[32];
      b = cl_nth(cls, 0, &len); memcpy(tmp, b, len); tmp[len] = 0;
      check_str("item0", tmp, "alpha");
      b = cl_nth(cls, 2, &len); memcpy(tmp, b, len); tmp[len] = 0;
      check_str("item2", tmp, "gamma");
      b = cl_nth(cls, 3, &len); check_int("item3 oob", b ? 1 : 0, 0);
    }
    check_int("count leading/trailing ws", cl_count(cls), 3);

    /* empty attribute edge cases */
    set_class(&cls, "");
    check_int("empty count", cl_count(cls), 0);
    cl_write_add(&cls, "first", 5);
    check_str("add to empty", cls, "first");
    cl_write_remove(&cls, "first", 5);
    check_str("remove last -> empty", cls, "");

    free(cls);
    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
