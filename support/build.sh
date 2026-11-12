#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
mkdir -p .build
cd .build
rm -f cms*.o liblcms2.a
"${CC:-cc}" -std=c99 -O1 -DHasTHREADS=0 -I"$root/include" -I"$root/src" -c "$root"/src/cms*.c
ar rcs liblcms2.a cms*.o
"${CC:-cc}" -std=c99 -O1 -DHasTHREADS=0 -I"$root/include" -I"$root/src" "$root/support/baseline.c" liblcms2.a -lm -o baseline
"${CC:-cc}" -std=c99 -O1 -DHasTHREADS=0 -I"$root/include" -I"$root/src" "$root/support/old_driver.c" "$root/testbed/testplugin.c" "$root/testbed/zoo_icc.c" liblcms2.a -lm -o old_tests
printf 'LCMS_OFFLINE_BUILD_OK\n'
