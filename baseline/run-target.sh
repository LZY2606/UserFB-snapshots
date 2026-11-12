set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash baseline/build.sh "$build"
cc -std=c99 -O1 -I. tests/resp3_stream.c baseline/target_driver.c "$build/libhiredis.a" -lm -o "$build/target"
"$build/target"
cc -std=c99 -O1 -I. baseline/old_units.c "$build/libhiredis.a" -lm -o "$build/old"
"$build/old"
