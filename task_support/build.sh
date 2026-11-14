#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/ffmpeg
if [[ ! -f .task-build/ffmpeg/ffbuild/config.mak ]]; then
  if ! (cd .task-build/ffmpeg && ../../configure \
    --disable-everything --disable-autodetect --disable-x86asm \
    --disable-doc --disable-debug --disable-network --disable-shared --enable-static \
    --enable-ffmpeg --enable-ffprobe --enable-avdevice --enable-pthreads \
    --enable-indev=lavfi --enable-protocol=file,pipe \
    --enable-demuxer=rawvideo,pcm_f32le,nut --enable-muxer=rawvideo,pcm_f32le,nut,framecrc,null \
    --enable-decoder=rawvideo,pcm_f32le,pcm_f64le,wrapped_avframe \
    --enable-encoder=rawvideo,pcm_f32le \
    --enable-filter=color,aevalsrc,xfade,acrossfade,format,aformat,asetnsamples,settb,setpts,asettb,asetpts,trim,atrim,split,asplit,null,anull,scale,aresample \
    > ../configure.log 2>&1); then
    tail -30 .task-build/configure.log; exit 1
  fi
fi
if ! make -C .task-build/ffmpeg -j 4 ffmpeg ffprobe > .task-build/build.log 2>&1; then
  tail -30 .task-build/build.log; exit 1
fi
cc -std=c17 -O2 -Wall -Wextra task_support/baseline_check.c -o .task-build/baseline_check -lm
