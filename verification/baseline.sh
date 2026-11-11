#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c11 -O2 -I. verification/baseline.c -o "$b/encode"
"$b/encode" "$b/out.264" "$b/ref.yuv"
ffmpeg -v error -i "$b/out.264" -f rawvideo -pix_fmt yuv420p "$b/decoded.yuv"
[ "$(wc -c < "$b/decoded.yuv" | tr -d ' ')" = 18432 ]
cmp "$b/ref.yuv" "$b/decoded.yuv"
echo 'baseline: 18432 decoded bytes equal encoder reconstruction'
