#!/bin/sh
#
# Full verification for the intra reference-sample handling.
#
#   1. Builds libde265 plus the intra border regression from the *unmodified*
#      source tree and requires it to pass.
#   2. Repeats the same run on an unmodified temporary copy (control copy).
#   3. Creates three independent temporary source copies and injects one local
#      fault into each:
#        - ignore tile isolation,
#        - ignore constrained intra prediction,
#        - reverse the direction of reference-sample substitution.
#      Every faulty copy must still *compile*; the regression must then fail on
#      a pixel/reference-edge assertion and report the failing case and the
#      first difference.
#   4. Decodes a real WPP bitstream through the public API
#      (de265_push_data/de265_decode) with one thread and with four workers and
#      compares every frame plane by plane against FFmpeg's output.
#
# Builds and fault copies live in a temporary directory; the source tree is
# never written to.  Run it as:  bash verification/check_intra_border.sh

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/check_intra_border.XXXXXX")
trap 'rm -rf "$work"' EXIT

jobs=${JOBS:-4}
CXX=${CXX:-c++}

fail() {
  echo "check_intra_border: $*" >&2
  exit 1
}

# ---------------------------------------------------------------------------
# Build helpers
# ---------------------------------------------------------------------------

configure_and_build() {   # $1 = source dir, $2 = build dir, $3 = log tag
  src=$1
  build=$2
  tag=$3

  cmake -S "$src" -B "$build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_SDL=OFF \
        -DENABLE_SIMD=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_TESTS=ON \
        >"$work/$tag-configure.log" 2>&1 || {
    tail -n 40 "$work/$tag-configure.log" >&2
    fail "cmake configure failed for $src"
  }

  cmake --build "$build" -j "$jobs" --target intra_border_reference \
        >"$work/$tag-build.log" 2>&1 || {
    tail -n 60 "$work/$tag-build.log" >&2
    fail "build failed for $src"
  }
}

# Exact literal replacement with an expected number of occurrences (no regex
# escaping surprises, no fuzzy patch matching).
replace_literal() {       # file, old, new, expected_count, description
  OLD="$2" NEW="$3" EXPECT="$4" perl -0pi -e '
    my $old = $ENV{OLD};
    my $new = $ENV{NEW};
    my $count = 0;
    my $pos = 0;
    while (($pos = index($_, $old, $pos)) >= 0) {
      substr($_, $pos, length($old)) = $new;
      $pos += length($new);
      $count++;
    }
    END { exit($count == $ENV{EXPECT} ? 0 : 1); }
  ' "$1" || fail "could not inject fault '$5' into $1"
}

# ---------------------------------------------------------------------------
# Fault injections (each is a plausible, minimal local bug)
# ---------------------------------------------------------------------------

mutate_tile_isolation() {
  file=$1/libde265/intrapred.h
  replace_literal "$file" \
    'leftCTBSlice != currCTBSlice  || leftCTBTileID != currCTBTileID )' \
    'leftCTBSlice != currCTBSlice )' 1 \
    'ignore tile isolation (left)'
  replace_literal "$file" \
    'topCTBSlice  != currCTBSlice  || topCTBTileID  != currCTBTileID )' \
    'topCTBSlice  != currCTBSlice )' 1 \
    'ignore tile isolation (top)'
  replace_literal "$file" \
    'topleftCTBSlice !=currCTBSlice||topleftCTBTileID!=currCTBTileID )' \
    'topleftCTBSlice !=currCTBSlice )' 1 \
    'ignore tile isolation (top-left)'
  replace_literal "$file" \
    'toprightCTBSlice!=currCTBSlice||toprightCTBTileID!=currCTBTileID)' \
    'toprightCTBSlice!=currCTBSlice)' 1 \
    'ignore tile isolation (top-right)'
}

mutate_constrained_intra() {
  file=$1/libde265/intrapred.h
  replace_literal "$file" \
    'if (pps->constrained_intra_pred_flag) {' \
    'if (0) {' 3 \
    'ignore constrained intra prediction'
}

mutate_substitution_direction() {
  file=$1/libde265/intrapred.h
  replace_literal "$file" \
'      if (!available[-2*nT]) {
        out_border[-2*nT] = firstValue;
      }

      for (int i=-2*nT+1; i<=2*nT; i++)
        if (!available[i]) {
          out_border[i]=out_border[i-1];
        }' \
'      if (!available[2*nT]) {
        out_border[2*nT] = firstValue;
      }

      for (int i=2*nT-1; i>=-2*nT; i--)
        if (!available[i]) {
          out_border[i]=out_border[i+1];
        }' 1 \
    'reverse the missing-sample propagation direction'
}

# ---------------------------------------------------------------------------
# Test execution helpers
# ---------------------------------------------------------------------------

run_regression() {        # $1 = build dir, $2 = log file  -> returns test status
  "$1/tests/intra_border_reference" >"$2" 2>&1
}

print_coverage() {        # $1 = log file
  echo "  border cases:     $(grep -c '^COVER border' "$1" || true)"
  echo "  prediction cases: $(grep -c '^PRED' "$1" || true)"
  echo "  $(grep '^RESULT' "$1" || true)"
  echo "  covered boundary combinations (from the test log):"
  grep -E '^(COVER|PRED)' "$1" | sed 's/^/    /'
}

# ---------------------------------------------------------------------------
# 1. Unmodified source tree
# ---------------------------------------------------------------------------

echo '=== 1. intra border regression on the unmodified source tree ==='
configure_and_build "$root" "$work/pristine-build" pristine

if run_regression "$work/pristine-build" "$work/pristine.log"; then
  echo 'PASS: unmodified source tree'
  print_coverage "$work/pristine.log"
else
  cat "$work/pristine.log"
  fail 'the regression fails on the unmodified source tree'
fi

# ---------------------------------------------------------------------------
# 2. Unmodified copy (control) and the three faulty copies
# ---------------------------------------------------------------------------

echo '=== 2. control copy and fault-injection copies ==='

# A copy that is not modified must behave exactly like the original.
cp -R "$root" "$work/control"
configure_and_build "$work/control" "$work/control-build" control
if ! run_regression "$work/control-build" "$work/control.log"; then
  cat "$work/control.log"
  fail 'the unmodified copy does not pass'
fi
echo 'PASS: unmodified copy (control)'

run_fault_case() {        # $1 = number, $2 = description, $3 = mutate function
  number=$1
  description=$2
  mutate=$3

  copy="$work/fault$number"
  build="$work/fault$number-build"
  log="$work/fault$number.log"

  cp -R "$root" "$copy"
  "$mutate" "$copy"

  # The faulty source must still compile; a compile error is not a detection.
  configure_and_build "$copy" "$build" "fault$number"

  if run_regression "$build" "$log"; then
    cat "$log"
    fail "fault '$description' was not detected: the regression still passes"
  fi

  if ! grep -q '^FAIL case=' "$log"; then
    cat "$log"
    fail "fault '$description' did not fail on an assertion"
  fi

  first_failure=$(grep -m1 '^FAIL case=' "$log")
  echo "DETECTED fault '$description':"
  echo "  $first_failure"
}

run_fault_case 1 'ignore tile isolation'              mutate_tile_isolation
run_fault_case 2 'ignore constrained intra prediction' mutate_constrained_intra
run_fault_case 3 'reverse missing-sample propagation'  mutate_substitution_direction

# ---------------------------------------------------------------------------
# 3. Full-stream WPP regression through the public API
# ---------------------------------------------------------------------------

echo '=== 3. full-stream WPP regression (de265_push_data/de265_decode) ==='

command -v ffmpeg >/dev/null 2>&1 || fail 'ffmpeg is required for the stream regression'

ffmpeg -v error -f lavfi -i 'testsrc2=size=128x128:rate=3' -frames:v 4 \
       -c:v libx265 -preset ultrafast \
       -x265-params 'wpp=1:bframes=0:frame-threads=1:ctu=32:log-level=error' \
       -f hevc "$work/wpp.h265" \
       || fail 'could not encode the WPP test stream (libx265 required)'

ffmpeg -v error -i "$work/wpp.h265" -pix_fmt yuv420p -f rawvideo "$work/ffmpeg.yuv" \
       || fail 'could not decode the WPP test stream with FFmpeg'

"$CXX" -std=c++17 -O1 -I"$root" -I"$work/pristine-build" \
       "$root/verification/wpp_stream.cc" \
       "$work/pristine-build/libde265/libde265.a" -lpthread \
       -o "$work/wpp_stream" \
       || fail 'could not build the WPP stream witness'

"$work/wpp_stream" "$work/wpp.h265" "$work/ffmpeg.yuv" >"$work/wpp_stream.log" 2>&1 \
       || { cat "$work/wpp_stream.log"; fail 'the WPP stream regression failed'; }
cat "$work/wpp_stream.log"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo '=== summary ==='
echo "unmodified source tree: PASS ($(grep '^RESULT' "$work/pristine.log"))"
echo 'unmodified copy:        PASS'
for number in 1 2 3; do
  line=$(grep -m1 '^FAIL case=' "$work/fault$number.log")
  case $number in
    1) name='ignore tile isolation' ;;
    2) name='ignore constrained intra prediction' ;;
    3) name='reverse missing-sample propagation' ;;
  esac
  echo "fault $number ($name): DETECTED"
  echo "  $line"
done
echo "WPP stream:             PASS ($(grep -m1 'WPP regression' "$work/wpp_stream.log" 2>/dev/null || true))"
echo 'All checks completed.'
