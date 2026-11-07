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
	-I"$ROOT/user/lib/tz" -I"$ROOT/user/lib/desktop" -I"$ROOT/user/lib/ntp" -I"$ROOT/user/lib/netconn" -I"$ROOT/user/lib/notify" -I"$ROOT/user/lib/mediactl" -I"$ROOT/user/apps/browser" -I"$ROOT/user/lib/rar"
	-I"$ROOT/user/apps/browser/js"
)

# Host compressors the btrfs test cross-checks against; each one is used only when pkg-config finds it.
host_defs=()
host_libs=()
for entry in ZLIB:zlib ZSTD:libzstd LZO:lzo2; do
	pkg-config --exists "${entry#*:}" 2>/dev/null || continue
	host_defs+=("-DHAVE_HOST_${entry%%:*}")
	host_libs+=($(pkg-config --libs "${entry#*:}"))
done
if pkg-config --exists lzo2 2>/dev/null; then
	host_defs+=(-I"$(dirname "$(pkg-config --variable=includedir lzo2)")")
fi

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
	if [ "$name" = tz ]; then
		extra=(-DTZ_CONFIG_PATH='"'"$BUILD/timezone"'"')
	fi
	if [ "$name" = vary ]; then
		extra=(-DCACHE_DIR='"'"$BUILD/vary-cache"'"')
	fi
	if [ "$name" = btrfs ] && [ ${#host_defs[@]} -gt 0 ]; then
		extra=("${host_defs[@]}")
	fi
	"$CC" -std=gnu11 "${INCLUDES[@]}" -Dmain=owos_main_${name} \
		-DOWOS_TEST_NAME=${name} -include "$ROOT/tests/test.h" \
		"${extra[@]}" -c "$source" -o "$BUILD/${name}.o" || exit 1
done

"$CC" -std=gnu11 "${INCLUDES[@]}" -DPERM_DB='"'"$BUILD/permissions.db"'"' \
	-c "$ROOT/user/apps/browser/features/permissions/permissions.c" -o "$BUILD/permissions_impl.o" || exit 1


"$CC" -std=gnu11 "${INCLUDES[@]}" -DCACHE_DIR='"'"$BUILD/vary-cache"'"' \
	-Dmkdir=vary_mkdir -Dunlink=vary_unlink -Dreaddir=vary_readdir -include "$ROOT/tests/vary_fs.h" \
	-c "$ROOT/user/apps/browser/features/cache/cache.c" -o "$BUILD/cache_impl.o" || exit 1

"$CC" -std=gnu11 -I"$ROOT/tests" -DCACHE_DIR='"'"$BUILD/vary-cache"'"' \
	-c "$ROOT/tests/vary_fs.c" -o "$BUILD/vary_fs.o" || exit 1

"$CC" -std=gnu11 "${INCLUDES[@]}" -DTZ_CONFIG_PATH='"'"$BUILD/timezone"'"' \
	-c "$ROOT/user/lib/tz/tz.c" -o "$BUILD/tz_impl.o" || exit 1

"$CC" -std=gnu11 "${INCLUDES[@]}" -Dreaddir=desktop_test_readdir -include "$ROOT/tests/desktop_fs.h" \
	-c "$ROOT/user/lib/desktop/desktop.c" -o "$BUILD/desktop_impl.o" || exit 1

"$CC" -std=gnu11 "${INCLUDES[@]}" -c "$ROOT/tests/browser_js_host.c" \
	-o "$BUILD/browser_js_host.o" || exit 1

"$CC" -std=gnu11 "${INCLUDES[@]}" -c "$ROOT/tests/test_runner.c" \
	-o "$BUILD/test_runner.o" || exit 1

"$CC" "${INCLUDES[@]}" "$BUILD"/*.o \
	"$ROOT/user/apps/browser/css/engine.c" "$ROOT/user/apps/browser/css/engine_values.c" "$ROOT/user/apps/browser/css/engine_props.c" "$ROOT/user/apps/browser/css/engine_decl.c" "$ROOT/user/apps/browser/css/engine_sel.c" "$ROOT/user/apps/browser/css/engine_sheet.c" "$ROOT/user/apps/browser/css/sibling.c" "$ROOT/user/apps/browser/color.c" \
	"$ROOT/user/apps/browser/dom.c" "$ROOT/user/apps/browser/features/csp/csp.c" \
	"$ROOT/user/apps/browser/js/classlist.c" \
	"$ROOT/user/apps/browser/js/webapi.c" \
	"$ROOT/user/apps/browser/features/notification/notification.c" \
	"$ROOT/user/lib/curl/url.c" "$ROOT/user/lib/curl/httpcache.c" \
	"$ROOT/user/lib/curl/headers.c" "$ROOT/user/lib/crypto/crypto.c" \
	"$ROOT/user/lib/base64/base64.c" "$ROOT/user/lib/zlib/crc32.c" "$ROOT/kernel/crc32c.c" \
	"$ROOT/user/lib/stun/stun.c" "$ROOT/user/lib/ntp/ntp.c" "$ROOT/user/lib/mediactl/session.c" "$ROOT"/user/lib/rar/*.c "$ROOT/drivers/usb/hid_desc.c" "$ROOT/user/lib/mp4/mp4.c" \
	"$ROOT/user/lib/sqlite/sqlite.c" \
	"$ROOT/user/lib/js/lexer.c" "$ROOT/user/lib/js/parser.c" \
	"$ROOT/user/lib/js/ast.c" "$ROOT/user/lib/js/value.c" \
	"$ROOT/user/lib/js/interp.c" "$ROOT/user/lib/js/builtins.c" \
	"$ROOT/user/lib/js/bigint.c" "$ROOT/user/lib/math/fmt.c" \
	"$ROOT/user/lib/gc/gc.c" "$ROOT/scripts/host_math.c" "$ROOT/user/lib/opt.c" "$ROOT/user/lib/mutex.c" \
	"${host_libs[@]}" -lm -lpthread -o "$BUILD/owos-tests" || exit 1

exec "$BUILD/owos-tests"
