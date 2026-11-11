set -eu
if b=$(mktemp -d 2>/dev/null); then
	:
else
	# fallback for sandboxes where the default mktemp path is not writable
	b="${TMPDIR:-/tmp}/libltc-baseline.$$"
	mkdir -m 700 "$b"
fi
trap 'rm -rf "$b"' EXIT
${CC:-cc} -std=c99 -O2 -Isrc tools/baseline.c src/ltc.c src/decoder.c src/encoder.c src/timecode.c -lm -o "$b/baseline"
"$b/baseline"
${CC:-cc} -std=c99 -O2 -Isrc tests/ltcloop.c src/ltc.c src/decoder.c src/encoder.c src/timecode.c -lm -o "$b/ltcloop"
"$b/ltcloop"
printf '%s\n' BASELINE_LTC_UPSTREAM_LOOP_OK
