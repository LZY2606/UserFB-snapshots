#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-cache
BUILD=$(mktemp -d "$PWD/.task-cache/check-target.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT
CXX=${CXX:-c++}
sources=()
for f in *.cpp; do [[ "$f" == main.cpp ]] || sources+=("$f"); done
"$CXX" -std=c++20 -O1 -I. task_support/main.cpp task_tests/target.cpp "${sources[@]}" -o "$BUILD/target" > "$BUILD/build.log" 2>&1 || { cat "$BUILD/build.log"; exit 1; }
set +e
"$BUILD/target" > "$BUILD/run.log" 2>&1
status=$?
set -e
cat "$BUILD/run.log"
[[ "$status" -eq 0 ]] || exit "$status"
for group in topology zfail clipping lighting; do
 grep -Eq "^GROUP $group cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$BUILD/run.log"
done
grep -qx TASK_SUITE_COMPLETE "$BUILD/run.log"
"$CXX" -std=c++20 -O1 -I. examples/shadow_volume.cpp "${sources[@]}" -o "$BUILD/example"
mkdir -p .task-output/shadow-volume
rm -f .task-output/shadow-volume/shadow-mask.tga .task-output/shadow-volume/shadow-frame.tga
"$BUILD/example" "$PWD/.task-output/shadow-volume"
"$CXX" -std=c++20 -O1 -I. task_support/image_check.cpp tgaimage.cpp -o "$BUILD/check-images"
"$BUILD/check-images" "$PWD/.task-output/shadow-volume"
bash task_support/baseline.sh
