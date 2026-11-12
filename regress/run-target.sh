#!/usr/bin/env bash
set -euo pipefail
test -s regress/msadpcm_write_test.c || { echo 'missing regress/msadpcm_write_test.c';exit 1; }
b=$(mktemp -d);trap 'rm -rf "$b"' EXIT
cc -std=c99 -Wall -Wextra -I . regress/msadpcm_write_test.c -lm -o "$b/contract"
for n in encode blocks errors;do
 out=$("$b/contract" "$n");printf '%s\n' "$out"
 grep -Eq "^PASS $n checks=[1-9][0-9]*$" <<<"$out"
done
cc -std=c99 -Wall -Wextra -c tests/compilation/dr_wav.c -o "$b/old-compilation.o"
echo MSADPCM_WRITE_TARGET_OK
