set -euo pipefail
build=$(mktemp -d "$PWD/.history-target.XXXXXX")
trap 'rm -rf "$build"' EXIT
# Compile the required C entry directly. Missing, empty, or unlinked tests fail.
cc -std=c99 -O1 -Isrc -c test/history_model.c -o "$build/model.o"
cc -std=c99 -O1 -Isrc -c test/history_contract.c -o "$build/contract.o"
bash baseline/build.sh "$build/lib"
cc -std=c99 -O1 -Isrc baseline/target_driver.c "$build/model.o" "$build/contract.o" "$build/lib/libonig.a" -o "$build/target"
"$build/target" "$@"
cc -std=c99 -O1 -Isrc baseline/witness.c "$build/lib/libonig.a" -o "$build/witness"
"$build/witness"
"$build/lib/test/test_syntax" 2>&1
