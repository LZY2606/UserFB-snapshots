#!/usr/bin/env bash
#
# check_excitation_mutations.sh
#
# Mutation testing for tests/excitation_contract.c.  Two deliberately broken,
# still-compilable copies of the production sources are built in temporary
# directories and linked against the unchanged contract test:
#
#   A. ltp.c: the left/right (outer) 3-tap pitch gains are exchanged,
#      i.e. gain[2-i] is replaced by gain[i] in pitch_unquant_3tap().
#   B. cb_search.c: in split_cb_shape_sign_unquant(), a decoded negative
#      sign bit is applied as a positive sign (s=-1 becomes s=1), which
#      only changes the explicit-sign (wideband high-band) codebook path.
#
# Each mutated build must fail the numerical content comparison (the test
# prints CONTENT_MISMATCH and the task driver returns 4).  A compile error or
# a crash does not count as a detection.  The unmutated control build must
# still pass.  No production source in the working tree is modified.

set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

tmpdir=$(mktemp -d)
cleanup() {
   rm -rf "$tmpdir"
}
trap cleanup EXIT

log=$tmpdir/run.log
status=0

fail() {
   echo "EXCITATION_MUTATION_FAIL: $*" >&2
   status=1
}

# ----------------------------------------------------------------------
# 0. Unmutated control build must pass.
# ----------------------------------------------------------------------
bash devcheck/build.sh . tests/excitation_contract.c \
     "$tmpdir/control" task_support/driver.c -DTASK_OBSERVE
if ! "$tmpdir/control" >"$log" 2>&1; then
   cat "$log"
   fail "control build did not pass before mutation testing"
   exit 1
fi
if ! grep -q '^TARGET_COMPLETE ' "$log"; then
   cat "$log"
   fail "control build did not report TARGET_COMPLETE"
   exit 1
fi
echo "mutation control: PASS (unmutated contract still green)"

# ----------------------------------------------------------------------
# Shared helper: materialize an overlay source root with one mutated file.
# ----------------------------------------------------------------------
make_overlay() {
   local root=$1
   mkdir -p "$root"
   cp -R libspeex include task_support "$root/"
}

run_mutation() {
   local name=$1
   local root=$2
   shift 2
   local bin=$tmpdir/$name
   local out=$tmpdir/$name.log

   # Build the mutated library (compile failure is not a valid detection).
   if ! bash devcheck/build.sh "$root" "$REPO/tests/excitation_contract.c" \
        "$bin" "$REPO/task_support/driver.c" -DTASK_OBSERVE >"$out" 2>&1; then
      cat "$out"
      fail "$name: mutated source failed to compile"
      return
   fi

   # A crash (signal / non-4 status) is not a valid detection either.
   set +e
   "$bin" >"$out" 2>&1
   local rc=$?
   set -e
   if [[ $rc -ne 4 ]]; then
      cat "$out"
      fail "$name: expected driver exit 4, got $rc"
      return
   fi
   if ! grep -q 'CONTENT_MISMATCH' "$out"; then
      cat "$out"
      fail "$name: exit 4 without CONTENT_MISMATCH in the output"
      return
   fi
   echo "mutation $name: detected (exit 4, CONTENT_MISMATCH printed)"
}

# ----------------------------------------------------------------------
# Mutation A: swap outer tap gains in the adaptive codebook.
# ----------------------------------------------------------------------
rootA=$tmpdir/rootA
make_overlay "$rootA"
perl -0pi -e '
   s/MAC16_16\(exc_out\[j\],gain\[2-i\],exc\[j-pp\]\)/MAC16_16(exc_out[j],gain[i],exc[j-pp])/g;
   s/MAC16_16\(exc_out\[j\],gain\[2-i\],exc\[j-pp-pitch\]\)/MAC16_16(exc_out[j],gain[i],exc[j-pp-pitch])/g;
' "$rootA/libspeex/ltp.c"
hits=$(grep -c 'MAC16_16(exc_out\[j\],gain\[i\]' "$rootA/libspeex/ltp.c" || true)
if [[ $hits -ne 2 ]]; then
   fail "mutation A: expected 2 swapped gain sites, found $hits"
   exit 1
fi
# The working tree must remain untouched.
if ! git diff --quiet -- libspeex/ltp.c; then
   fail "mutation A leaked into the working tree (libspeex/ltp.c)"
   exit 1
fi
run_mutation A "$rootA"

# ----------------------------------------------------------------------
# Mutation B: explicit sign bit ignored (negative applied as positive).
# The s=-1 site is unique to split_cb_shape_sign_unquant().
# ----------------------------------------------------------------------
rootB=$tmpdir/rootB
make_overlay "$rootB"
# s=-1 is unique to the explicit-sign branch of split_cb_shape_sign_unquant.
perl -pi -e 's/^(\s*)s=-1;[ \t]*$/$1s=1;/' "$rootB/libspeex/cb_search.c"
if ! grep -qF 'if (signs[i])' "$rootB/libspeex/cb_search.c" || \
   grep -qF 's=-1;' "$rootB/libspeex/cb_search.c" || \
   ! grep -qE '^[[:space:]]+s=1;$' "$rootB/libspeex/cb_search.c"; then
   fail "mutation B: sign-flip edit did not apply as expected"
   exit 1
fi
if ! git diff --quiet -- libspeex/cb_search.c; then
   fail "mutation B leaked into the working tree (libspeex/cb_search.c)"
   exit 1
fi
run_mutation B "$rootB"

if [[ $status -eq 0 ]]; then
   echo "EXCITATION_MUTATION_OK both mutations detected via CONTENT_MISMATCH"
fi
exit $status
