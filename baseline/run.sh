set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash baseline/build.sh "$build"
cc -std=c99 -O1 -I. baseline/witness.c "$build/libhiredis.a" -lm -o "$build/witness"
"$build/witness"
cc -std=c99 -O1 -I. baseline/old_units.c "$build/libhiredis.a" -lm -o "$build/old"
"$build/old"
