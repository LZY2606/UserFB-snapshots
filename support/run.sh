#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
node support/baseline.mjs
case "${1:-baseline}" in
 baseline) exit 0;;
 target) ;;
 *) exit 2;;
esac
[ -s support/target.mjs ] || { echo 'missing support/target.mjs' >&2; exit 2; }
[ -s examples/affine-bake.mjs ] || { echo 'missing examples/affine-bake.mjs' >&2; exit 2; }
build=$(mktemp -d "${TMPDIR:-/tmp}/check.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
node --no-warnings --experimental-loader ./support/loader.mjs support/driver.mjs > "$build/target.log"
cat "$build/target.log"
for group in position normal records ownership; do
 grep -E "^EXECUTED $group cases=[1-9][0-9]* assertions=[1-9][0-9]*$" "$build/target.log" >/dev/null
done
grep -x 'TARGET_OK' "$build/target.log" >/dev/null
node --no-warnings --experimental-loader ./support/loader.mjs examples/affine-bake.mjs "$build/out" > "$build/example.log"
cat "$build/example.log"
[ -s "$build/example.log" ]
[ -s "$build/out/result.obj" ]
