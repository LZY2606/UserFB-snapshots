#!/bin/bash
set -euo pipefail
mkdir -p .task-build/x264
if [[ ! -f .task-build/x264/config.mak ]]; then
 (cd .task-build/x264 && ../../configure --disable-cli --disable-asm --enable-static --disable-opencl --disable-thread --disable-interlaced --bit-depth=8 --chroma-format=420 --disable-swscale --disable-lavf --disable-ffms --disable-gpac --disable-lsmash > configure.log 2>&1)
fi
# Keep the version SEI stable if a caller adds local Git commits.
version_config=.task-build/x264/x264_config.h
sed -E '/^#define X264_(REV|REV_DIFF|VERSION|POINTVER)( |$)/d' "$version_config" > "$version_config.fixed"
cat task_support/version-frozen.h >> "$version_config.fixed"
if ! cmp -s "$version_config" "$version_config.fixed"; then
  mv "$version_config.fixed" "$version_config"
else
  rm "$version_config.fixed"
fi
make -s -C .task-build/x264 -j2 libx264.a > .task-build/build.log 2>&1
