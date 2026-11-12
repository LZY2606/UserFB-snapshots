set -euo pipefail
build=$(mktemp -d "$PWD/.history-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
bash baseline/build.sh "$build"
cc -std=c99 -O1 -Isrc baseline/witness.c "$build/libonig.a" -o "$build/witness"
"$build/witness"
"$build/test/test_syntax" 2>&1
