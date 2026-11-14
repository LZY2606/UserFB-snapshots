#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash repro/build.sh
flags=(-std=c99 -O2 -UNDEBUG -I. -Iinclude -Ilib -Ithird_party/ogg/include)
for name in granulepos comment; do
  cc "${flags[@]}" "tests/$name.c" .task-build/libcodec.a -lm -o ".task-build/$name"
  ".task-build/$name"
done
cc "${flags[@]}" repro/stream_smoke.c .task-build/libcodec.a -lm -o .task-build/stream_smoke
.task-build/stream_smoke
printf 'THEORA_BASELINE_OK granulepos=6 comment=1 stream=4\n'
