#!/usr/bin/env bash
set -euo pipefail
root=$PWD
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
sources=(lib/marisa/agent.cc lib/marisa/keyset.cc lib/marisa/trie.cc lib/marisa/grimoire/trie/louds-trie.cc lib/marisa/grimoire/trie/tail.cc lib/marisa/grimoire/vector/bit-vector.cc lib/marisa/grimoire/io/mapper.cc lib/marisa/grimoire/io/reader.cc lib/marisa/grimoire/io/writer.cc)
objects=()
for f in "${sources[@]}"; do o="$build/$(basename "$f").o"; c++ -std=c++17 -O1 -Iinclude -Ilib -c "$f" -o "$o"; objects+=("$o"); done
c++ -std=c++17 -O1 -Iinclude -Ilib regress/baseline.cc "${objects[@]}" -o "$build/baseline"
if [[ ${1:-baseline} == target ]]; then
 c++ -std=c++17 -O1 -Iinclude -Ilib -Iregress regress/main.cc tests/fuzzy_target.cc "${objects[@]}" -o "$build/target"
 "$build/target" > "$build/target.log"
 cat "$build/target.log"
 grep -qx 'MARISA_TARGET_COMPLETE groups=4' "$build/target.log"
elif [[ ${1:-baseline} != baseline ]]; then exit 2; fi
"$build/baseline"
c++ -std=c++17 -O1 -Iinclude -Ilib -Itests tests/trie-test.cc "${objects[@]}" -o "$build/trie-test"
(cd "$build" && ./trie-test)
echo 'MARISA_LEGACY_COMPLETE'
