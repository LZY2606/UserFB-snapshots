#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s src/test/java/org/jcodec/containers/mp4/demuxer/EditedPlaybackTest.java
  test -s src/main/java/org/jcodec/containers/mp4/demuxer/EditedMP4Demuxer.java
  test -s samples/EditedPlayback.java
fi
mkdir -p .task-build
bash task_support/build.sh > .task-build/compile.log 2>&1
if [[ $mode == target ]]; then
  javac --release 8 -encoding UTF-8 -cp .task-build/classes -d .task-build/classes task_support/Binding.java src/test/java/org/jcodec/containers/mp4/demuxer/EditedPlaybackTest.java samples/EditedPlayback.java
  java -cp .task-build/classes:src/main/resources task_support.Baseline
  java -cp .task-build/classes:src/main/resources task_support.Binding
  rm -f .task-build/example-video.bin .task-build/example-pcm.bin
  java -cp .task-build/classes:src/main/resources EditedPlayback .task-build/edited-input.mp4 .task-build/example-video.bin .task-build/example-pcm.bin
  test -s .task-build/example-video.bin
  test -s .task-build/example-pcm.bin
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  java -cp .task-build/classes:src/main/resources task_support.Driver > "$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline seekAtomic ioCompat; do
    grep -Eq "^GROUP ${group} checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Fxq 'TARGET_COMPLETE groups=3' "$output_log"
else
  java -cp .task-build/classes:src/main/resources task_support.Baseline --observe-gap
fi
