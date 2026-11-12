#!/usr/bin/env bash
set -euo pipefail
test -s regress/filter_contract.c || { echo 'missing regress/filter_contract.c';exit 1; }
test -s regress/check-mutations.sh || { echo 'missing regress/check-mutations.sh';exit 1; }
b=$(mktemp -d);trap 'rm -rf "$b"' EXIT
bash regress/build.sh "$b"
cc -std=c99 -Wall -Wextra -I src regress/filter_contract.c "$b/src/libsoxr.a" -lm -o "$b/filter-contract"
for n in passband alias phase;do
 out=$("$b/filter-contract" "$n");printf '%s\n' "$out"
 grep -Eq "^PASS $n checks=[1-9][0-9]*$" <<<"$out"
done
ctest --test-dir "$b" -R '^1-delay-clear$' --output-on-failure
bash regress/check-mutations.sh
echo SOXR_FILTER_CONTRACT_TARGET_OK
