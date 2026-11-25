#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/karaoke_bake.c; fi
mkdir -p .task-build
./configure --disable-all --enable-isoff --enable-isoff-write --enable-isoff-frag --enable-ttxt --enable-dasher --enable-odf --static-build --disable-opt --disable-x11 --use-zlib=no --disable-opensvc --disable-openhevc --disable-platinum --disable-freetype --disable-ssl --disable-jpeg --disable-openjpeg --disable-png --disable-mad --disable-a52 --disable-xvid --disable-faad --disable-ffmpeg --disable-freenect --disable-vorbis --disable-theora --disable-nghttp2 --disable-ngtcp2 --disable-nghttp3 --disable-oss --disable-dvb4linux --disable-alsa --disable-pulseaudio --disable-jack --disable-directfb --disable-hid --disable-lzma --disable-tinygl --disable-vtb --disable-ogg --disable-sdl --disable-caption --disable-mpeghdec --disable-libcaca --disable-curl > .task-build/configure.log 2>&1
rm -f bin/gcc/libgpac_static.a
make -C src -j2 all > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
if [[ $mode == target ]]; then
  cc -std=c99 -DGPAC_HAVE_CONFIG_H -I. -Iinclude -Iunittests task_support/driver.c tests/karaoke_bake.c bin/gcc/libgpac_static.a -lm -lpthread -o .task-build/target
test -s examples/tx3g_bake.c
cc -std=c99 -DGPAC_HAVE_CONFIG_H -I. -Iinclude -Iunittests examples/tx3g_bake.c bin/gcc/libgpac_static.a -lm -lpthread -o .task-build/tx3g_bake
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in event_intervals style_resolution movie_roundtrip; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -DGPAC_HAVE_CONFIG_H -I. -Iinclude -Iunittests task_support/baseline.c bin/gcc/libgpac_static.a -lm -lpthread -o .task-build/baseline
.task-build/baseline
