#!/usr/bin/env bash

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-cc}
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/owos-tests.XXXXXX") || exit 1
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM

INCLUDES=(
	-I"$ROOT/scripts" -I"$ROOT/tests" -I"$ROOT/user/lib"
	-I"$ROOT/user/lib/js" -I"$ROOT/user/lib/math" -I"$ROOT/user/lib/gc"
	-I"$ROOT/user/lib/base64" -I"$ROOT/user/lib/curl"
	-I"$ROOT/user/lib/crypto" -I"$ROOT/user/lib/resolve"
	-I"$ROOT/user/lib/zlib" -I"$ROOT/user/lib/stun" -I"$ROOT/user/lib/mp4"
	-I"$ROOT/user/lib/sqlite" -I"$ROOT/user/lib/gfx" -I"$ROOT/user/lib/png"
	-I"$ROOT/user/lib/netconn" -I"$ROOT/user/lib/notify" -I"$ROOT/user/apps/gui/browser"
)

for source in "$ROOT"/tests/*_test.c; do
	[ -f "$source" ] || continue
	if ! grep -q '^#define OWOS_SUITE_TEST$' "$source"; then
		continue
	fi
	file=${source##*/}
	name=${file%_test.c}
	extra=()
	if [ "$name" = permissions ]; then
		extra=(-DPERM_DB='"'"$BUILD/permissions.db"'"')
	fi
	"$CC" -std=gnu11 "${INCLUDES[@]}" -Dmain=owos_main_${name} \
		-DOWOS_TEST_NAME=${name} -include "$ROOT/tests/test.h" \
		"${extra[@]}" -c "$source" -o "$BUILD/${name}.o" || exit 1
done

"$CC" -std=gnu11 "${INCLUDES[@]}" -DPERM_DB='"'"$BUILD/permissions.db"'"' \
	-c "$ROOT/user/apps/gui/browser/permissions.c" -o "$BUILD/permissions_impl.o" || exit 1


"$CC" -std=gnu11 "${INCLUDES[@]}" -c "$ROOT/tests/browser_js_host.c" \
	-o "$BUILD/browser_js_host.o" || exit 1

"$CC" -std=gnu11 "${INCLUDES[@]}" -c "$ROOT/tests/test_runner.c" \
	-o "$BUILD/test_runner.o" || exit 1

"$CC" "${INCLUDES[@]}" "$BUILD"/*.o \
	"$ROOT/user/apps/gui/browser/css.c" "$ROOT/user/apps/gui/browser/color.c" \
	"$ROOT/user/apps/gui/browser/dom.c" "$ROOT/user/apps/gui/browser/csp.c" \
	"$ROOT/user/apps/gui/browser/classlist.c" \
	"$ROOT/user/apps/gui/browser/js_webapi.c" \
	"$ROOT/user/apps/gui/browser/js_notification.c" \
	"$ROOT/user/lib/curl/url.c" "$ROOT/user/lib/curl/httpcache.c" \
	"$ROOT/user/lib/curl/headers.c" "$ROOT/user/lib/crypto/crypto.c" \
	"$ROOT/user/lib/base64/base64.c" "$ROOT/user/lib/zlib/crc32.c" \
	"$ROOT/user/lib/stun/stun.c" "$ROOT/user/lib/mp4/mp4.c" \
	"$ROOT/user/lib/sqlite/sqlite.c" \
	"$ROOT/user/lib/js/lexer.c" "$ROOT/user/lib/js/parser.c" \
	"$ROOT/user/lib/js/ast.c" "$ROOT/user/lib/js/value.c" \
	"$ROOT/user/lib/js/interp.c" "$ROOT/user/lib/js/builtins.c" \
	"$ROOT/user/lib/js/bigint.c" "$ROOT/user/lib/math/fmt.c" \
	"$ROOT/user/lib/gc/gc.c" "$ROOT/scripts/host_math.c" \
	-lm -o "$BUILD/owos-tests" || exit 1

exec "$BUILD/owos-tests"
