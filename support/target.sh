#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash support/build.sh
# Compile and call the new C entry points, even when the source is empty.
"${CC:-cc}" -std=c99 -O1 -DHasTHREADS=0 -Iinclude -Isrc -Isupport \
    -include support/check.h support/target_driver.c \
    testbed/reverse_index.c .build/liblcms2.a -lm -o .build/target_tests
./.build/target_tests > .build/target.stdout
cat .build/target.stdout
for group in branches singular index; do
    grep -Eq "^UNIT_GROUP_PASS $group checks=[1-9][0-9]*$" .build/target.stdout
done
grep -Fxq 'UNIT_TARGET_COMPLETE groups=3' .build/target.stdout
bash support/baseline.sh
printf 'LCMS_REVERSE_TARGET_AND_OLD_OK\n'
