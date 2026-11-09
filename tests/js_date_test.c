/* Host test for the JS engine's Date (user/lib/js/builtins.c). Runs an assertion program through the real engine
 * with now_ms pinned to a fixed instant, so Date.now()/new Date() are deterministic. The engine's clock is UTC
 * (getTimezoneOffset === 0), so local and UTC accessors alias; the assertions reflect that, not a bug. */
#define OWOS_SUITE_TEST
#include "ast.h"
#include "gc.h"
#include "interp.h"
#include "parser.h"
#include "test.h"
#include "value.h"
#include <stdio.h>
#include <stdlib.h>

/* a fixed instant: 2021-06-15T10:20:30.000Z */
#define FIXED_NOW_MS 1623752430000LL
static long long fixed_now(void) { return FIXED_NOW_MS; }

static void con_print(js_ctx *ctx, int level, const char *s, int len)
{
  (void)ctx;
  (void)level;
  fprintf(stderr, "[console] %.*s\n", len, s);
}

/* eq() compares String(got) to want and collects every mismatch, so one run reports all of them; the program
 * exposes `count` (assertions run) and `fails` (newline-joined mismatches) as globals. */
static const char *PROGRAM =
    "var GMT = ' GMT+0000 (Coordinated Universal Time)';\n"
    "var count = 0, fails = [];\n"
    "function eq(got, want, label) {\n"
    "  count++;\n"
    "  if (String(got) !== String(want)) fails.push(label + ' => ' + got + ' (want ' + want + ')');\n"
    "}\n"
    "eq(Date.now(), 1623752430000, 'Date.now');\n"
    "eq(new Date().getTime(), 1623752430000, 'new Date() uses now_ms');\n"
    "eq(new Date(0).toISOString(), '1970-01-01T00:00:00.000Z', 'epoch ISO');\n"
    "eq(new Date(0).toString(), 'Thu Jan 01 1970 00:00:00' + GMT, 'epoch toString');\n"
    "eq(new Date(0).toUTCString(), 'Thu, 01 Jan 1970 00:00:00 GMT', 'epoch toUTCString');\n"
    "eq(new Date(0).toDateString(), 'Thu Jan 01 1970', 'epoch toDateString');\n"
    "eq(new Date(0).toTimeString(), '00:00:00' + GMT, 'epoch toTimeString');\n"
    "eq(new Date(0).getDay(), 4, 'epoch getDay');\n"
    "eq(new Date(2021, 0, 15, 12, 30, 45, 500).toISOString(), '2021-01-15T12:30:45.500Z', 'components ctor');\n"
    "eq(new Date(2021, 13, 1).toISOString(), '2022-02-01T00:00:00.000Z', 'month 13 overflow');\n"
    "eq(new Date(2021, -1, 1).toISOString(), '2020-12-01T00:00:00.000Z', 'month -1 underflow');\n"
    "eq(new Date(2021, 0, 32).toISOString(), '2021-02-01T00:00:00.000Z', 'day overflow');\n"
    "eq(new Date(99, 0, 1).getFullYear(), 1999, '2-digit year');\n"
    "eq(Date.UTC(2000, 0, 1), 946684800000, 'Date.UTC');\n"
    "eq(Date.parse('2021-06-15'), Date.UTC(2021, 5, 15), 'parse ISO date');\n"
    "eq(Date.parse('2021-06-15T10:20:30Z'), Date.UTC(2021, 5, 15, 10, 20, 30), 'parse ISO datetime');\n"
    "eq(Date.parse('2021-06-15T10:20:30+02:00'), Date.UTC(2021, 5, 15, 8, 20, 30), 'parse ISO +offset');\n"
    "eq(Date.parse('1970-01-01T00:00:00.250Z'), 250, 'parse ISO ms');\n"
    "eq(Date.parse('Thu, 01 Jan 1970 00:00:00 GMT'), 0, 'parse RFC-2822');\n"
    "eq(Date.parse('Thu Jan 01 1970 00:00:00 GMT+0000'), 0, 'parse toString form');\n"
    "eq(Date.parse('Jan 1, 2020'), Date.UTC(2020, 0, 1), 'parse Mon D, Y');\n"
    "eq(Date.parse('06/15/2021'), Date.UTC(2021, 5, 15), 'parse US M/D/Y');\n"
    "eq(new Date('2021-06-15T10:20:30Z').getTime(), Date.UTC(2021, 5, 15, 10, 20, 30), 'string ctor');\n"
    "eq(new Date(new Date(12345)).getTime(), 12345, 'copy ctor');\n"
    "eq(new Date(NaN).toString(), 'Invalid Date', 'invalid toString');\n"
    "eq(new Date(NaN).getTime(), NaN, 'invalid getTime');\n"
    "eq(new Date(NaN).getFullYear(), NaN, 'invalid getFullYear');\n"
    "eq(new Date(NaN).toJSON(), null, 'invalid toJSON');\n"
    "eq(isNaN(Date.parse('')), true, 'parse empty');\n"
    "eq(isNaN(Date.parse('garbage')), true, 'parse garbage');\n"
    "eq(Object.prototype.toString.call(new Date(0)), '[object Date]', 'toStringTag');\n"
    "eq((new Date(0)) instanceof Date, true, 'instanceof');\n"
    "eq((new Date(0)).constructor === Date, true, 'constructor');\n"
    "eq(Date.length, 7, 'ctor arity');\n"
    "eq(typeof Date(), 'string', 'Date() plain call');\n"
    "eq(new Date(2021, 0, 1).getYear(), 121, 'getYear legacy');\n"
    "eq(new Date(0) + '', 'Thu Jan 01 1970 00:00:00' + GMT, 'default-hint coercion');\n"
    "eq(String(new Date(0)), 'Thu Jan 01 1970 00:00:00' + GMT, 'String()');\n"
    "eq(new Date(5000) - new Date(2000), 3000, 'numeric subtraction');\n"
    "eq('' + new Date(5000) === String(new Date(5000)), true, 'concat matches String');\n"
    "eq(JSON.stringify({d:new Date(0)}), '{\"d\":\"1970-01-01T00:00:00.000Z\"}', 'JSON.stringify via toJSON');\n"
    "eq(new Date(Date.UTC(-1, 0, 1)).toISOString(), '-000001-01-01T00:00:00.000Z', 'negative extended year');\n"
    "eq(new Date(Date.UTC(275760, 8, 13)).toISOString(), '+275760-09-13T00:00:00.000Z', 'max extended year');\n"
    "eq(new Date(8.64e15 + 1).getTime(), NaN, 'over-range clips to NaN');\n"
    "eq(new Date(8.64e15).getTime(), 8640000000000000, 'max valid time');\n"
    "eq(new Date(NaN).toLocaleString(), 'Invalid Date', 'invalid toLocaleString');\n"
    "var intlThrew = false;\n"
    "try { new Intl.DateTimeFormat().format(new Date(NaN)); } catch (e) { intlThrew = e instanceof RangeError; }\n"
    "eq(intlThrew, true, 'Intl format of Invalid Date throws RangeError');\n"
    /* mutating setters and the Invalid-Date recovery/keep rules */
    "var s = new Date(0);\n"
    "s.setFullYear(2022); s.setMonth(5); s.setDate(20); s.setHours(13, 14, 15, 16);\n"
    "eq(s.toISOString(), '2022-06-20T13:14:15.016Z', 'chained setters');\n"
    "s.setTime(0); eq(s.getTime(), 0, 'setTime');\n"
    "var r = new Date(NaN); r.setFullYear(2000);\n"
    "eq(r.toISOString(), '2000-01-01T00:00:00.000Z', 'setFullYear recovers Invalid Date');\n"
    "var k = new Date(NaN); k.setHours(5);\n"
    "eq(isNaN(k.getTime()), true, 'setHours keeps Invalid Date');\n"
    "var wrap = new Date(Date.UTC(2020, 0, 31)); wrap.setMonth(1);\n"
    "eq(wrap.toISOString(), '2020-03-02T00:00:00.000Z', 'setMonth day overflow wraps');\n"
    "var thrown = false;\n"
    "try { new Date(NaN).toISOString(); } catch (e) { thrown = (e instanceof RangeError); }\n"
    "eq(thrown, true, 'toISOString throws RangeError on Invalid Date');\n"
    "this.count = count;\n"
    "this.fails = fails.join('\\n');\n";

/* eq() calls in PROGRAM; a lower count means the program stopped early. */
#define ASSERTIONS 53

static void run_date_program(void)
{
  js_arena arena;
  js_parse_error err = {0, 0};
  js_value result;

  js_arena_init(&arena);
  js_ctx *ctx = js_ctx_new(con_print);
  ctx->now_ms = fixed_now;
  js_node *prog = js_parse(PROGRAM, &arena, &err);
  expect(prog != 0);
  if (prog && js_run(ctx, prog, &result) != 0) {
    js_value msg = js_get(ctx, ctx->exc, "message");
    expect_string(msg.type == JV_STR ? msg.u.str->data : "(non-string exception)", "");
  } else if (prog) {
    js_value count = js_get(ctx, js_obj_value(ctx->global), "count");
    js_value fails = js_get(ctx, js_obj_value(ctx->global), "fails");
    expect_eq(count.type == JV_NUM ? (int)count.u.num : -1, ASSERTIONS);
    expect_string(fails.type == JV_STR ? fails.u.str->data : "(fails missing)", "");
  }
  js_ctx_free(ctx);
  js_arena_free(&arena);
}

int main(void)
{
  test("Date semantics", run_date_program);
  return 0;
}
