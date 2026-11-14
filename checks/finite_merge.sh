#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
"${CXX:-clang++}" -std=c++11 -O1 -g -fsanitize-coverage=inline-8bit-counters,pc-table checks/merge_fixture.cpp .task-build/libFuzzer.a -pthread -o .task-build/merge-fixture
fixture_dir=$(mktemp -d ".task-build/finite-merge.XXXXXXXX")
trap 'rm -rf "$fixture_dir"' EXIT
mkdir "$fixture_dir/old" "$fixture_dir/new"
printf A > "$fixture_dir/old/one"
printf AB > "$fixture_dir/new/two"
printf ABC > "$fixture_dir/new/three"
printf ZZZ > "$fixture_dir/new/four"
.task-build/merge-fixture -merge=1 -merge_control_file="$fixture_dir/control.txt" "$fixture_dir/old" "$fixture_dir/new"
test -s "$fixture_dir/control.txt"
test "$(find "$fixture_dir/old" -type f | wc -l)" -gt 1
echo FINITE_BENIGN_MERGE_COMPLETE
