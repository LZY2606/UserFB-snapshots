#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
[[ "$mode" == baseline || "$mode" == target ]] || exit 2
if [[ "$mode" == target ]]; then
  for file in elfio/elfio_bpf_graph.hpp tests/bpf_graph_cases.cpp examples/bpf_graph/bpf_graph.cpp; do
    [[ -s "$file" ]] || { echo "MISSING_TARGET $file" >&2; exit 1; }
  done
fi
bpf_build=$(mktemp -d)
trap 'rm -rf "$bpf_build"' EXIT
cxx=${CXX:-c++}
flags=(-std=c++17 -O1 -I.)
"$cxx" "${flags[@]}" task_support/make_sample.cpp -o "$bpf_build/make_sample"
"$cxx" "${flags[@]}" task_support/raw_records.cpp -o "$bpf_build/raw_records"
"$cxx" "${flags[@]}" examples/writer/writer.cpp -o "$bpf_build/writer"
"$cxx" "${flags[@]}" examples/tutorial/tutorial.cpp -o "$bpf_build/tutorial"
mkdir "$bpf_build/samples"
"$bpf_build/make_sample" "$bpf_build/samples"
for object in one two; do
  "$bpf_build/raw_records" "$bpf_build/samples/$object.o"
done
(cd "$bpf_build"; ./writer; ./tutorial hello_x86_64 > tutorial.log)
grep -q 'ELF' "$bpf_build/tutorial.log"
echo UPSTREAM_WRITER_TUTORIAL_OK
if [[ "$mode" == target ]]; then
  # Include the new public header through the public umbrella, independently of tests.
  printf '#include <elfio/elfio.hpp>\n#include <elfio/elfio_bpf_graph.hpp>\nint main(){}\n' > "$bpf_build/include.cpp"
  "$cxx" "${flags[@]}" "$bpf_build/include.cpp" -o "$bpf_build/include"
  "$cxx" "${flags[@]}" tests/bpf_graph_cases.cpp task_support/driver.cpp -o "$bpf_build/target"
  "$cxx" "${flags[@]}" examples/bpf_graph/bpf_graph.cpp -o "$bpf_build/example"
  set +e
  "$bpf_build/target" > "$bpf_build/target.log" 2>&1
  status=$?
  set -e
  cat "$bpf_build/target.log"
  [[ "$status" == 0 ]] || exit "$status"
  for group in identity flow provenance; do
    grep -Eq "^GROUP $group cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$bpf_build/target.log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$bpf_build/target.log"
  for object in one two; do
    "$bpf_build/example" "$bpf_build/samples/$object.o" "$bpf_build/$object"
    for tag in F C M H W; do grep -Eq "^${tag}[[:space:]]" "$bpf_build/$object.tsv"; done
    grep -Eq 'digraph[[:space:]]' "$bpf_build/$object.dot"
    test -s "$bpf_build/$object.dot"
  done
  echo BPF_GRAPH_DEMO_OK objects=2
fi
