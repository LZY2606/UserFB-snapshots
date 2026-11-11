#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b/build" -DCMAKE_BUILD_TYPE=Release -DENABLE_SDL=OFF -DENABLE_SIMD=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build "$b/build" -j 4
ffmpeg -v error -f lavfi -i 'testsrc2=size=128x96:rate=3' -frames:v 3 -c:v libx265 -preset ultrafast -x265-params 'pools=none:frame-threads=1:bframes=0:log-level=error' -f hevc "$b/clip.h265"
c++ -std=c++17 -O1 -I. -I"$b/build" verification/baseline.cc "$b/build/libde265/libde265.a" -lpthread -o "$b/witness"
"$b/witness" "$b/clip.h265" "$b/libde265.yuv"
ffmpeg -v error -i "$b/clip.h265" -pix_fmt yuv420p -f rawvideo "$b/ffmpeg.yuv"
[ "$(wc -c < "$b/libde265.yuv" | tr -d ' ')" = 55296 ]
cmp "$b/libde265.yuv" "$b/ffmpeg.yuv"
echo 'baseline: 3 HEVC frames, 55296 bytes equal FFmpeg'
