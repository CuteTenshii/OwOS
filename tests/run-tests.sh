#!/usr/bin/env bash
# Build (incrementally, in parallel) and run the host tests.
#   tests/run-tests.sh [-v] [-l] [-jN] [suite...]
# Objects are cached in build/tests; JOBS overrides the compile parallelism.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 1
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
make -s -f tests/tests.mk -j"$JOBS" build || exit 1

# Suites write their databases under build/tests/run; start every run from an empty one
rm -rf build/tests/run && mkdir -p build/tests/run
status=0
ran=0
for bin in owos-tests owos-bindings-tests; do
	build/tests/$bin "$@"
	rc=$?
	[ $rc -ne 2 ] && ran=1
	[ $rc -ne 0 ] && [ $rc -ne 2 ] && status=1
done
if [ $ran -eq 0 ]; then
	echo "no such test suite: $*" >&2
	exit 1
fi
exit $status
