#!/bin/sh
# regression for the stream-capable compressor (arbitrary chunk sizes, continuous control
# periods, delay-tail draining). every group prints its own maximum error and tail frame count,
# and any failed assertion makes this script exit non-zero
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

# an explicit template keeps mktemp on the system temp dir (a bare mktemp -d ignores $TMPDIR on
# macOS and can point at an unwritable path)
b=$(mktemp -d "${TMPDIR:-/tmp}/compstream.XXXXXX")
trap 'rm -rf "$b"' EXIT

# C-level regression (it carries its own independent copy of the old whole-period algorithm)
cc -std=gnu99 -Werror -I"$root/src" \
	"$root/checks/compressor_stream.c" "$root/src/compressor.c" -lm -o "$b/ctest"

# the real file-processing entry point, so the WAV path exercises src/main.c end to end
cc -std=gnu99 -fwrapv -Werror -I"$root/src" \
	"$root/src/main.c" "$root/src/mem.c" "$root/src/snd.c" "$root/src/wav.c" \
	"$root/src/biquad.c" "$root/src/compressor.c" "$root/src/reverb.c" \
	-lm -o "$b/sndfilter"

echo "== C regression groups =="
"$b/ctest"

D=$("$b/ctest" latency 48000)
echo "== WAV end-to-end via sndfilter CLI (reported latency D=$D) =="

# lengths cover 0, single frame, one frame short of a period, a full period, and one over, plus
# larger aligned and unaligned streams; none of these rely on chunk alignment inside the library
for N in 0 1 31 32 33 100 193 288; do
	"$b/ctest" wavgen "$b/in-$N.wav" "$N" 48000
	"$b/sndfilter" "$b/in-$N.wav" "$b/out-$N.wav" compressor 0 -24 30 12 0.003 0.250
	if [ "$N" -eq 0 ]; then
		expect=0
		expecttail=0
	else
		expect=$((N + D))
		expecttail=$D
	fi
	"$b/ctest" wavcheck "$b/out-$N.wav" "$expect" "$N" "$expecttail"
done

echo "compressor_stream: all checks passed"
