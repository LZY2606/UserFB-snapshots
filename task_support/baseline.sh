#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=.task-build/ffmpeg
"$b/ffmpeg" -v error -nostdin -filter_complex_threads 1 \
 -f lavfi -i 'color=c=black:s=8x8:r=25:d=1,format=gray' \
 -f lavfi -i 'color=c=white:s=8x8:r=25:d=1,format=gray' \
 -f lavfi -i 'aevalsrc=0.25|-0.125:s=48000:d=1' \
 -f lavfi -i 'aevalsrc=-0.5|0.375:s=48000:d=1' \
 -filter_complex '[0:v][1:v]xfade=transition=fade:duration=0.2:offset=0.8[v];[2:a][3:a]acrossfade=d=0.2:c1=tri:c2=tri,aformat=sample_fmts=flt[a]' \
 -map '[v]' -map '[a]' -c:v rawvideo -c:a pcm_f32le -fps_mode passthrough -y .task-build/baseline.nut
"$b/ffmpeg" -v error -nostdin -i .task-build/baseline.nut -map 0:v -c:v copy -f rawvideo -y .task-build/baseline.gray
"$b/ffmpeg" -v error -nostdin -i .task-build/baseline.nut -map 0:a -c:a copy -f f32le -y .task-build/baseline.f32
"$b/ffprobe" -v error -show_entries stream=codec_type,time_base -of compact .task-build/baseline.nut
.task-build/baseline_check .task-build/baseline.gray .task-build/baseline.f32
