#!/bin/bash
# tests/check_phase_mutations.sh
#
# Mutation-detection proof for tests/phase_overlap_contract.c (see
# PHASE_TESTS.md).  Two still-runnable degradations are applied to a
# temporary copy of the source tree and the same contract suite is run
# against each copy:
#
#   clock_short        the fundamental clock advances one sample less
#                      per frame (src/phase.c)
#   overlap_head_early the overlap head starts one sample earlier in
#                      the inverse transform (src/sine.c)
#
# A mutation counts as detected only when the contract runner prints
# the first CONTENT_MISMATCH and exits 4.  A compile failure or an
# abnormal process exit (any other status) does not count.

set -uo pipefail
cd "$(dirname "$0")/.."
root=$(pwd)

work=$(mktemp -d "${TMPDIR:-/tmp}/phase-mut.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

copy_tree() {
  dst="$1"
  mkdir -p "$dst" || return 1
  for item in CMakeLists.txt codec2.pc.in README.md COPYING cmake src demo \
              tests task_support; do
    cp -R "$root/$item" "$dst/" || return 1
  done
}

fail=0

run_mutation() {
  name="$1"; file="$2"; script="$3"
  dst="$work/$name"
  if ! copy_tree "$dst"; then
    echo "MUTATION $name ERROR: could not copy source tree"
    fail=1
    return
  fi
  target="$dst/$file"
  cp "$target" "$target.orig"
  sed "$script" "$target" > "$target.mut" && mv "$target.mut" "$target"
  if cmp -s "$target.orig" "$target"; then
    echo "MUTATION $name ERROR: pattern did not change $file"
    fail=1
    return
  fi
  rm -f "$target.orig"
  ( cd "$dst" && bash task_support/run.sh target ) > "$work/$name.log" 2>&1
  rc=$?
  if [ "$rc" -eq 4 ] && grep -q "CONTENT_MISMATCH" "$work/$name.log"; then
    echo "MUTATION $name detected (runner=4): $(grep -m1 CONTENT_MISMATCH "$work/$name.log")"
  else
    echo "MUTATION $name NOT DETECTED (runner=$rc, expected 4 with CONTENT_MISMATCH)"
    fail=1
  fi
}

# degradation 1: fundamental clock advances one sample less per frame
run_mutation clock_short src/phase.c \
  's/ex_phase\[0\] += (model->Wo) \* n_samp;/ex_phase[0] += (model->Wo) * (n_samp - 1);/'

# degradation 2: overlap head starts one sample earlier in the inverse
# transform
run_mutation overlap_head_early src/sine.c \
  's/sw_\[FFT_DEC - n_samp + 1 + i\]/sw_[FFT_DEC - n_samp + i]/'

if [ "$fail" -eq 0 ]; then
  echo "PHASE_MUTATIONS_OK"
fi
exit "$fail"
