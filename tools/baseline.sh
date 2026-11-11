set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
${CC:-cc} -std=c99 -O2 -Isrc tools/baseline.c src/ltc.c src/decoder.c src/encoder.c src/timecode.c -lm -o "$b/baseline"
"$b/baseline"
${CC:-cc} -std=c99 -O2 -Isrc tests/ltcloop.c src/ltc.c src/decoder.c src/encoder.c src/timecode.c -lm -o "$b/ltcloop"
"$b/ltcloop"
printf '%s\n' BASELINE_LTC_UPSTREAM_LOOP_OK
