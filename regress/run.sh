set -euo pipefail
mode=${1:-baseline}
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
flags=(-std=gnu11 -O2 -Iinclude -Isrc -Iregress/generated -Iregress/compat -Itest/runner -Iregress)
objects=()
for s in src/*.c src/syscalls/*.c regress/generated/*.c test/runner/interpreter.c; do
  o="$b/$(basename "$s").o"
  cc "${flags[@]}" -c "$s" -o "$o"
  objects+=("$o")
done
if [ "$mode" = baseline ]; then
 cc "${flags[@]}" regress/baseline.c "${objects[@]}" -o "$b/case"
else
 cc "${flags[@]}" regress/harness.c regress/target.c "${objects[@]}" -o "$b/case"
fi
"$b/case" >"$b/out"
cat "$b/out"
if [ "$mode" != baseline ]; then
 bash regress/check-output.sh "$b/out"
 cc "${flags[@]}" regress/baseline.c "${objects[@]}" -o "$b/old"
 "$b/old"
fi
