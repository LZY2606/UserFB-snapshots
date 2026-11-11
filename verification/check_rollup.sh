#!/bin/sh
# Self contained roll-up window regression check.
# Everything is compiled and executed inside a temporary directory, so the
# script can be run straight from the source tree:  bash verification/check_rollup.sh
set -eu

b=$(mktemp -d "${TMPDIR:-/tmp}/libcaption-rollup.XXXXXX")
trap 'rm -rf "$b"' EXIT

root=$(cd "$(dirname "$0")/.." && pwd)
cp "$root/src/eia608_from_utf8.c.cached" "$b/eia608_from_utf8.c"

core_srcs="$root/src/caption.c $root/src/cea708.c $root/src/eia608.c \
    $root/src/eia608_charmap.c $root/src/utf8.c $root/src/xds.c \
    $b/eia608_from_utf8.c"
flags="-std=c99 -Wall -Wextra -O1 -I$root -I$root/caption"

# New roll-up window tests (direct decode and CEA708 wrapper, cell by cell).
# shellcheck disable=SC2086
cc $flags "$root/unit_tests/test_rollup_window.c" $core_srcs -o "$b/test_rollup_window"
"$b/test_rollup_window"

# The existing text wrapping test must keep building and running.
# shellcheck disable=SC2086
cc $flags "$root/unit_tests/test_wrap.c" "$root/src/utf8.c" -o "$b/test_wrap"
"$b/test_wrap" >/dev/null

# The baseline witness now locks in the corrected window behaviour.
# shellcheck disable=SC2086
cc $flags "$root/verification/baseline.c" $core_srcs -o "$b/baseline"
"$b/baseline"

echo "check_rollup: all checks passed"
