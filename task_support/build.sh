#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
objects=()
for source in mkvmuxer/*.cc mkvparser/*.cc common/file_util.cc common/hdr_util.cc; do
  object=".task-build/obj/${source//\//_}.o"
  c++ -std=c++11 -O0 -g -I. -c "$source" -o "$object"
  objects+=("$object")
done
ar rcs .task-build/libwebm.a "${objects[@]}"
c++ -std=c++11 -O0 -I. task_support/baseline.cc .task-build/libwebm.a -o .task-build/baseline
c++ -std=c++11 -O0 -I. mkvmuxer_sample.cc sample_muxer_metadata.cc webvtt/webvttparser.cc webvtt/vttreader.cc .task-build/libwebm.a -o .task-build/mkvmuxer_sample
