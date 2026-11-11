#!/bin/sh
# End-to-end verification for H264E_run_param_t::mb_qp (per-macroblock QP map).
#
#   1. builds verification/check_mb_qp.c (scenario generator, linked against
#      the library) and verification/mb_qp_dump.c (standalone CAVLC QP tracer)
#   2. runs the scenarios: checkerboard map, padded map, two threads, NALU
#      slicing, all-zero-map compatibility, invalid-map rejection + retry,
#      map non-inheritance
#   3. decodes every produced stream with FFmpeg and compares against the
#      encoder's local reconstruction (must be byte-identical)
#   4. independently reads per-macroblock QP from the bitstreams with
#      mb_qp_dump and checks the map landed on the right macroblocks
#
# Usage: bash verification/check_mb_qp.sh
set -eu

b=$(mktemp -d "${TMPDIR:-/tmp}/check_mb_qp.XXXXXX")
trap 'rm -rf "$b"' EXIT

cc -std=c11 -O2 -I. verification/check_mb_qp.c -o "$b/check_mb_qp"
cc -std=c11 -O2 -I. verification/mb_qp_dump.c -o "$b/mb_qp_dump"

# scenario streams + in-process checks (compat/retry/inherit/invalid maps)
"$b/check_mb_qp" "$b"

# every stream: FFmpeg decode must equal the encoder's local reconstruction
for f in checker padding threads slices compat retry; do
    ffmpeg -v error -i "$b/$f.264" -f rawvideo -pix_fmt yuv420p "$b/$f.dec.yuv"
    cmp "$b/$f.ref.yuv" "$b/$f.dec.yuv"
    echo "$f: decoded output identical to encoder reconstruction"
done

# map scenarios: independent QP readout must match the expected map
for f in checker padding threads slices; do
    "$b/mb_qp_dump" "$b/$f.264" "$b/$f.expect" | tail -1 | sed "s/^/$f: /"
done

echo 'check_mb_qp: all checks passed'
