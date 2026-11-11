#!/bin/sh
#
# tests/check-sync-clock.sh
#
# Builds libltc and tests/sync-clock.c in a temporary directory and runs the
# independent synchronization regression tests.
#
# Without arguments it only verifies the unmodified library.
# With --mutations it additionally builds three independent copies of the
# library sources, applies one behavior-degrading change to each, and requires
# the test binary to detect every one of them through its assertions.
#
# A mutation counts as caught only when the modified library compiles, the
# test binary terminates normally with the test-failure exit status (1) and
# prints ASSERT_FAIL lines. Compile failures, crashes and timeouts are not
# accepted as detections. The original src/ tree is never modified.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$script_dir/.." && pwd)

cc=${CC:-cc}
SRCS="ltc.c decoder.c encoder.c timecode.c"
HDRS="ltc.h decoder.h encoder.h"
TEST_SRC="$repo/tests/sync-clock.c"
RUN_TIMEOUT=30

make_tmpdir() {
	if command -v gmktemp >/dev/null 2>&1; then
		gmktemp -d
	elif command -v mktemp >/dev/null 2>&1; then
		mktemp -d 2>/dev/null || {
			d="${TMPDIR:-/tmp}/ltc-sync.$$"
			mkdir -m 700 "$d"
			echo "$d"
		}
	else
		d="${TMPDIR:-/tmp}/ltc-sync.$$"
		mkdir -m 700 "$d"
		echo "$d"
	fi
}

if command -v timeout >/dev/null 2>&1; then
	TIMEOUT_BIN=timeout
elif command -v gtimeout >/dev/null 2>&1; then
	TIMEOUT_BIN=gtimeout
else
	TIMEOUT_BIN=""
fi

if ! command -v perl >/dev/null 2>&1; then
	echo "perl is required to apply the mutations" >&2
	exit 2
fi

tmp=$(make_tmpdir)
trap 'rm -rf "$tmp"' EXIT INT TERM

copy_sources() {
	dest=$1
	mkdir -p "$dest"
	for f in $SRCS; do
		cp "$repo/src/$f" "$dest/$f"
	done
	for f in $HDRS; do
		cp "$repo/src/$f" "$dest/$f"
	done
}

build_test() {
	srcdir=$1
	out=$2
	# shellcheck disable=SC2086
	"$cc" -std=c99 -O2 -Wall -I"$srcdir" "$TEST_SRC" \
		"$srcdir"/ltc.c "$srcdir"/decoder.c "$srcdir"/encoder.c \
		"$srcdir"/timecode.c -lm -o "$out"
}

# run_test bin logfile -> sets RUN_STATUS to the (nonzero-safe) exit status
run_test() {
	bin=$1
	log=$2
	set +e
	if [ -n "$TIMEOUT_BIN" ]; then
		"$TIMEOUT_BIN" "${RUN_TIMEOUT}s" "$bin" >"$log" 2>&1
	else
		"$bin" >"$log" 2>&1
	fi
	RUN_STATUS=$?
	set -e
}

# mutation 1: the 1024-sample PCM conversion chunks stop accumulating posinfo
apply_offset_mutation() {
	dir=$1
	file="$dir/ltc.c"
	[ "$(grep -c 'posinfo + (ltc_off_t)copyStart' "$file")" -eq 1 ] || return 1
	perl -0777 -i -pe 's/\Qposinfo + (ltc_off_t)copyStart);\E/posinfo);/' "$file"
	grep -q 'decode_ltc(d, tmp, c, posinfo);' "$file" || return 1
	! grep -q 'posinfo + (ltc_off_t)copyStart' "$file" || return 1
}

# mutation 2: reverse playback drops the 16-bit sync-word position compensation
apply_reverse_mutation() {
	dir=$1
	file="$dir/decoder.c"
	[ "$(grep -c '16 \* d->snd_to_biphase_period' "$file")" -eq 2 ] || return 1
	perl -0777 -i -pe 's/\Qd->frame_start_off - 16 * d->snd_to_biphase_period;\E/d->frame_start_off;/' "$file"
	perl -0777 -i -pe 's/\Qposinfo + (ltc_off_t) offset - 1LL - 16 * d->snd_to_biphase_period;\E/posinfo + (ltc_off_t) offset - 1LL;/' "$file"
	! grep -q '16 \* d->snd_to_biphase_period' "$file" || return 1
	grep -q 'd->queue\[d->queue_write_off\].off_start = d->frame_start_off;' "$file" || return 1
}

# mutation 3: queue_flush additionally resets the bit counter of the parser
apply_flush_mutation() {
	dir=$1
	file="$dir/ltc.c"
	[ "$(grep -c 'd->queue_read_off = d->queue_write_off;' "$file")" -eq 1 ] || return 1
	perl -0777 -i -pe 's/\Qd->queue_read_off = d->queue_write_off;\E/d->queue_read_off = d->queue_write_off;\n\td->bit_cnt = 0;/' "$file"
	# the reset must be inside queue_flush, not added anywhere else
	grep -A2 'void ltc_decoder_queue_flush' "$file" | grep -q 'd->bit_cnt = 0;' || return 1
}

# evaluate_one variant_label srcdir expected_marker
evaluate_one() {
	label=$1
	srcdir=$2
	marker=$3
	bindir="$srcdir/bin"
	log="$srcdir/run.log"

	if ! build_test "$srcdir" "$bindir" 2>"$srcdir/build.log"; then
		echo "MUTATION $label: modified library failed to compile" >&2
		cat "$srcdir/build.log" >&2
		return 1
	fi

	run_test "$bindir" "$log"
	if [ "$RUN_STATUS" -ne 1 ]; then
		echo "MUTATION $label: not detected (exit status $RUN_STATUS," \
		     "wanted ordinary test failure 1)" >&2
		cat "$log" >&2
		return 1
	fi
	if ! grep -q '^ASSERT_FAIL' "$log"; then
		echo "MUTATION $label: binary failed without any ASSERT_FAIL" >&2
		cat "$log" >&2
		return 1
	fi

	echo "MUTATION $label: failing assertions:"
	sed -n 's/^ASSERT_FAIL \([A-Za-z0-9_.]*\).*/  \1/p' "$log" | sort -u
	echo "$marker"
}

# ---------------------------------------------------------------------
# Unmodified library must pass everything
# ---------------------------------------------------------------------
orig="$tmp/original"
copy_sources "$orig"
if ! build_test "$orig" "$orig/sync-clock" 2>"$orig/build.log"; then
	echo "original library failed to compile" >&2
	cat "$orig/build.log" >&2
	exit 1
fi
run_test "$orig/sync-clock" "$orig/run.log"
if [ "$RUN_STATUS" -ne 0 ] || ! grep -q 'LTC_SYNC_CLOCK_ALL_PASS' "$orig/run.log"; then
	echo "original library failed the sync-clock tests" >&2
	cat "$orig/run.log" >&2
	exit 1
fi
echo "LTC_SYNC_ORIGINAL_PASS"

if [ "${1:-}" != "--mutations" ]; then
	exit 0
fi

# ---------------------------------------------------------------------
# Three independent mutated source copies
# ---------------------------------------------------------------------
offset_dir="$tmp/mut-offset"
reverse_dir="$tmp/mut-reverse"
flush_dir="$tmp/mut-flush"

copy_sources "$offset_dir"
copy_sources "$reverse_dir"
copy_sources "$flush_dir"

apply_offset_mutation "$offset_dir"
apply_reverse_mutation "$reverse_dir"
apply_flush_mutation "$flush_dir"

fail=0
evaluate_one "offset"  "$offset_dir"  "LTC_SYNC_OFFSET_MUTATION_CAUGHT"  || fail=1
evaluate_one "reverse" "$reverse_dir" "LTC_SYNC_REVERSE_MUTATION_CAUGHT" || fail=1
evaluate_one "flush"   "$flush_dir"   "LTC_SYNC_FLUSH_MUTATION_CAUGHT"   || fail=1

if [ "$fail" -ne 0 ]; then
	echo "one or more mutations were not properly detected" >&2
	exit 1
fi

exit 0
