set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash baseline/build.sh "$build"
cc -std=c99 -O1 -I. tests/chunk_verify.c baseline/target_driver.c "$build/lua.o" -lm -o "$build/target"
"$build/target"
bash baseline/run.sh
