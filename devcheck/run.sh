set -euo pipefail
mode=${1:-baseline}
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
objects=()
for s in digest.c libkeccak_*.c extra/*.c hmac/*.c spec/*.c util/*.c; do
 o="$b/$(basename "$s").o"
 cc -std=c99 -O2 -I. -c "$s" -o "$o"
 objects+=("$o")
done
if [ "$mode" = baseline ]; then
 cc -std=c99 -O2 -I. devcheck/baseline.c "${objects[@]}" -o "$b/case"
else
 cc -std=c99 -O2 -I. -Idevcheck devcheck/harness.c devcheck/target.c "${objects[@]}" -o "$b/case"
fi
"$b/case" >"$b/out"
cat "$b/out"
if [ "$mode" != baseline ]; then bash devcheck/check-output.sh "$b/out"; fi
cc -std=c99 -O2 -I. test.c "${objects[@]}" -o "$b/old"
cp .testfile "$b/.testfile"
(cd "$b" && ./old >old.log 2>&1) || { cat "$b/old.log"; exit 1; }
printf '%s\n' KECCAK_OLD_TESTS_OK
