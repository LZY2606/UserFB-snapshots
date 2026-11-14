#!/bin/bash
set -euo pipefail
.task-build/legacy vectors/foreman.264 .task-build/plain.mp4 >.task-build/plain.log
cmp .task-build/plain.mp4 vectors/out_ref.mp4
.task-build/legacy -s vectors/foreman.264 .task-build/seq.mp4 >.task-build/seq.log
cmp .task-build/seq.mp4 vectors/out_sequential_ref.mp4
.task-build/legacy -f vectors/foreman.264 .task-build/frag.mp4 >.task-build/frag.log
cmp .task-build/frag.mp4 vectors/out_fragmentation_ref.mp4
.task-build/legacy vectors/foreman_slices.264 .task-build/slices.mp4 >.task-build/slices.log
cmp .task-build/slices.mp4 vectors/out_slices_ref.mp4
for kind in plain seq slices; do
  .task-build/legacy -d .task-build/$kind.mp4 .task-build/$kind.264 >.task-build/demux-$kind.log
  src=vectors/foreman.264
  if [[ $kind == slices ]]; then src=vectors/foreman_slices.264; fi
  cmp .task-build/$kind.264 "$src"
done
echo 'LEGACY_OK mux=4 demux=3'
