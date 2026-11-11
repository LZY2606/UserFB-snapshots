#!/bin/bash
# Runs the self-contained true-peak regression against the real library and
# against three temporary copies that each contain one deliberate degradation.
# It does not download audio and does not need libsndfile.
set -u

root=$(cd "$(dirname "$0")/.." && pwd) || exit 2
cc=${CC:-cc}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/true_peak_check.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT

test_c="$root/test/true_peak_test.c"
ref_c="$root/test/true_peak_reference.c"
ref_h="$root/test/true_peak_reference.h"
lib_c="$root/ebur128/ebur128.c"
lib_h="$root/ebur128/ebur128.h"

for f in "$test_c" "$ref_c" "$ref_h" "$lib_c" "$lib_h"; do
  if [ ! -s "$f" ]; then
    echo "MISSING_FILE: $f" >&2
    exit 2
  fi
done

compile_into() {
  # $1 = output executable, $2 = library .c, $3 = directory holding ebur128.h
  "$cc" -std=c99 -D_GNU_SOURCE -O2 -I "$3" -I "$root/test" \
    "$test_c" "$ref_c" "$2" -lm -o "$1"
}

# Compilation success is confirmed independently before any run is called a
# detection.  A compile failure or missing executable is never a detection.
confirm_compile() {
  local out=$1 log=$2
  shift 2
  if ! compile_into "$out" "$@" >"$log" 2>&1; then
    echo "COMPILE_FAILED: $log" >&2
    sed -n '1,40p' "$log" >&2
    return 1
  fi
  if [ ! -x "$out" ]; then
    echo "COMPILE_PRODUCED_NO_EXECUTABLE: $out" >&2
    return 1
  fi
  return 0
}

case_count() {
  awk -F= '/^CASES_RUN=/{split($2, a, " "); print a[1]}' "$1" | tail -n 1
}

# ---------------------------------------------------------------------------
# Baseline: the same numeric assertions must pass on the untouched library.
# ---------------------------------------------------------------------------
if ! confirm_compile "$tmp/original" "$tmp/original.compile.log" \
     "$lib_c" "$root/ebur128"; then
  exit 2
fi
if "$tmp/original" >"$tmp/original.log" 2>&1; then
  baseline_cases=$(case_count "$tmp/original.log")
  echo "BASELINE_ASSERTIONS_PASS cases=${baseline_cases:-unknown}"
else
  echo "ORIGINAL_ASSERTIONS_FAILED" >&2
  sed -n '1,80p' "$tmp/original.log" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# Each mutant is a fresh temporary copy of ebur128.c.  Only the library copy
# changes; the test sources and the reference stay identical.
# ---------------------------------------------------------------------------
detect_one() {
  local name=$1 marker_ere=$2 mut_dir=$3
  local out="$mut_dir/check" log="$mut_dir/run.log"
  local exit_code first_match cases

  if ! confirm_compile "$out" "$mut_dir/compile.log" \
       "$mut_dir/ebur128.c" "$mut_dir"; then
    echo "MUTANT_COMPILE_FAILED: $name" >&2
    return 1
  fi

  "$out" >"$log" 2>&1
  exit_code=$?
  if [ "$exit_code" -eq 0 ]; then
    echo "NOT_DETECTED: $name (test still passed)" >&2
    return 1
  fi
  first_match=$(grep -E -m 1 "ASSERT_FAIL .*${marker_ere}" "$log" || true)
  if [ -z "$first_match" ]; then
    echo "NOT_DETECTED: $name (numeric assertions failed, but expected marker '${marker_ere}' was absent)" >&2
    sed -n '1,20p' "$log" >&2
    return 1
  fi
  cases=$(case_count "$log")
  echo "DETECTED_${name}: ${first_match} (cases=${cases:-unknown})"
  return 0
}

# Degradation 1: the interpolator is never run; true peak collapses to the
# sample peak.  The cat1 assertion requires prev_true_peak > prev_sample_peak.
mut="$tmp/mutant_sample_only"
mkdir -p "$mut"
cp "$lib_h" "$mut/ebur128.h"
awk '{ if ($0 ~ /ebur128_check_true_peak\(st, frames\);/) { print "      /* degradation: skip interpolation */                             \\" } else print }' \
  "$lib_c" >"$mut/ebur128.c"
if ! detect_one SAMPLE_ONLY 'cat1_prev_true_exceeds_sample' "$mut"; then
  echo "CHECK_FALSE_POSITIVE_CHAIN:sample_only" >&2
  exit 1
fi

# Degradation 2: interpolation history is cleared at every add_frames()
# boundary instead of flowing across calls.  cat3 puts non-zero history on
# the call-1 side of the boundary and its strongest peak on the call-2 side.
mut="$tmp/mutant_history_reset"
mkdir -p "$mut"
cp "$lib_h" "$mut/ebur128.h"
awk '
/while \(frames > 0\) \{/ {
  print "    if (st->d->interp) {                                                     \\";
  print "      unsigned int zc, zt;                                                   \\";
  print "      for (zc = 0; zc < st->d->interp->channels; ++zc) {                     \\";
  print "        for (zt = 0; zt < st->d->interp->delay; ++zt) {                      \\";
  print "          st->d->interp->z[zc][zt] = 0.0f;                                   \\";
  print "        }                                                                    \\";
  print "      }                                                                      \\";
  print "      st->d->interp->zi = 0;                                                 \\";
  print "    }                                                                        \\";
}
{ print }
' "$lib_c" >"$mut/ebur128.c"
if ! detect_one HISTORY_RESET 'cat3_two_calls.*call2_prev_true' "$mut"; then
  echo "CHECK_FALSE_POSITIVE_CHAIN:history_reset" >&2
  exit 1
fi

# Degradation 3: channel c>0 takes channel 0's interpolated output for the
# true-peak statistics.  cat2 has a large negative pulse on channel 0 and an
# independent small signal on channel 1.
mut="$tmp/mutant_channel_alias"
mkdir -p "$mut"
cp "$lib_h" "$mut/ebur128.h"
awk '{ if ($0 ~ /resampler_buffer_output\[i \* st->channels \+ c\];/) { print "          (double) st->d->resampler_buffer_output[i * st->channels + (c > 0 ? 0 : c)];" } else print }' \
  "$lib_c" >"$mut/ebur128.c"
if ! detect_one CHANNEL_ALIAS 'cat2_channel_isolation' "$mut"; then
  echo "CHECK_FALSE_POSITIVE_CHAIN:channel_alias" >&2
  exit 1
fi

echo "TRUE_PEAK_REFERENCE_OK"
echo "DETECTED_SAMPLE_ONLY"
echo "DETECTED_HISTORY_RESET"
echo "DETECTED_CHANNEL_ALIAS"
echo "cases_baseline=${baseline_cases:-unknown}"
