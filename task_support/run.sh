#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/mux_comment.cpp; fi
cmake -S . -B .task-build/taglib -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_BINDINGS=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DWITH_ZLIB=OFF
cmake --build .task-build/taglib --target tag -j 4
c++ -std=c++11 -DTAGLIB_STATIC -I. -Itaglib -Itaglib/toolkit -Itaglib/ogg -Itaglib/ogg/vorbis -Itaglib/flac -I.task-build/taglib task_support/baseline.cpp .task-build/taglib/taglib/libtag.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -DTAGLIB_STATIC -I. -Itaglib -Itaglib/toolkit -Itaglib/ogg -Itaglib/ogg/vorbis -Itaglib/flac -I.task-build/taglib -x c++ task_support/driver.c tests/mux_comment.cpp -x none .task-build/taglib/taglib/libtag.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in selection rewrite boundaries; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
