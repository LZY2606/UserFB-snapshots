set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash baseline/build.sh "$build"
cc -std=c99 -O1 -I. baseline/witness.c "$build/lua.o" -lm -o "$build/witness"
"$build/witness"
