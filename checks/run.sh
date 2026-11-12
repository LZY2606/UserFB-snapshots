#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) echo "unknown check mode" >&2; exit 2;; esac
b=$(mktemp -d "${TMPDIR:-/tmp}/elfio-btf.XXXXXX")
trap 'rm -rf "$b"' EXIT
cxx=${CXX:-c++}
"$cxx" -std=c++17 -O2 -I. checks/btf_witness.cpp -o "$b/btf"
"$b/btf"
"$cxx" -std=c++17 -O2 -I. checks/symbols_witness.cpp -o "$b/symbols"
"$b/symbols"
"$cxx" -std=c++17 -O2 -I. examples/writer/writer.cpp -o "$b/writer"
"$cxx" -std=c++17 -O2 -I. examples/tutorial/tutorial.cpp -o "$b/tutorial"
(cd "$b" && ./writer && ./tutorial hello_x86_64) > "$b/upstream.log"
cat "$b/upstream.log"
grep -q '^ELF file class    : ELF64$' "$b/upstream.log"
grep -q '^ELF file encoding : Little endian$' "$b/upstream.log"
echo UPSTREAM_WRITER_TUTORIAL_OK
if [[ $mode == target ]]; then
  test -s tests/btf_compaction.cpp || { echo 'missing tests/btf_compaction.cpp' >&2; exit 1; }
  "$cxx" -std=c++17 -O2 -I. checks/btf_driver.cpp tests/btf_compaction.cpp -o "$b/target"
  "$cxx" -std=c++17 -O2 -I. examples/btf_compact/btf_compact.cpp -o "$b/example"
  "$b/target" > "$b/target.log" 2>&1 || { cat "$b/target.log"; exit 1; }
  cat "$b/target.log"
  for group in cycles distinctions ext_refs malformed roundtrip depth; do
    [[ $(grep -Ec "^PASS $group checks=[1-9][0-9]*$" "$b/target.log") == 1 ]] || { echo "incomplete group $group" >&2; exit 1; }
  done
  [[ $(grep -Ec '^PASS [a-z_]+ checks=[1-9][0-9]*$' "$b/target.log") == 6 ]]
  [[ $(grep -Ec '^BTF_COMPACTION_OK groups=6 checks=[1-9][0-9]*$' "$b/target.log") == 1 ]]
fi
