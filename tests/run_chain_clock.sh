#!/usr/bin/env bash
# Regression runner for tests/chain_clock.cc.
#
# 1. Verifies the pristine library passes all 8 case groups.
# 2. Applies each mutation from devcheck/mutations to an isolated copy of
#    the library sources, builds the same test binary against it, and
#    confirms the mutation is caught by the expected check:
#      - floor-clock.patch    -> per-chain sample length error (CLOCK_LENGTH)
#      - wrong-preroll.patch  -> sound reference error (PCM_REFERENCE), with
#                                all six chain length checks passing first
# Production sources are never modified; all work happens in a temp dir.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$PWD

cases=(
  queued-44100-20 queued-44100-60 queued-48000-20 queued-48000-60
  streamed-44100-20 streamed-44100-60 streamed-48000-20 streamed-48000-60
)

work=$(mktemp -d "$PWD/.chain-clock.XXXXXX")
trap 'rm -rf "$work"' EXIT

# --- pristine library: every group must pass -------------------------------
bash devcheck/build-test.sh tests/chain_clock.cc "$work/chain-clock"
for c in "${cases[@]}"; do
  if ! "$work/chain-clock" --case "$c" >"$work/normal-$c.log" 2>&1; then
    echo "PRISTINE_CASE_FAILED $c" >&2
    cat "$work/normal-$c.log" >&2
    exit 1
  fi
  grep -qx "PASS $c chains=6" "$work/normal-$c.log" || {
    echo "PRISTINE_CASE_NO_PASS $c" >&2; exit 1; }
done

# build_variant NAME PATCH: copy the library sources, apply the mutation,
# and build the unchanged test against the copy.  Any failure here (copy,
# patch, compile) aborts the script without reporting a detection.
build_variant() {
  local name=$1 patch_file=$2
  local dst="$work/$name"
  mkdir -p "$dst"
  cp -R "$repo/src" "$dst/src"
  cp -R "$repo/include" "$dst/include"
  (cd "$dst" && patch -p1 -s < "$repo/devcheck/mutations/$patch_file") || {
    echo "MUTATION_APPLY_FAILED $patch_file" >&2; exit 1; }
  bash devcheck/build-test.sh tests/chain_clock.cc "$dst/chain-clock" "$dst" || {
    echo "MUTATION_BUILD_FAILED $patch_file" >&2; exit 1; }
}

# --- mutation 1: cumulative end point rounded down instead of up -----------
build_variant floor floor-clock.patch
floor_detected=1
for c in "${cases[@]}"; do
  log="$work/floor-$c.log"
  status=pass
  "$work/floor/chain-clock" --case "$c" >"$log" 2>&1 || status=fail
  case "$c" in
    *-44100-*)
      # 44100 Hz: ceil vs floor differ, so the per-chain sample length
      # check must fail with a clock error.
      if [ "$status" != fail ] || ! grep -q "FAIL $c CLOCK_LENGTH" "$log"; then
        floor_detected=0
      fi
      ;;
    *-48000-*)
      # 48000 Hz: N*48000/rate is exact, so these groups must still pass;
      # this also proves the binary ran the real assertions.
      if [ "$status" != pass ] || ! grep -qx "PASS $c chains=6" "$log"; then
        floor_detected=0
      fi
      ;;
  esac
done
if [ "$floor_detected" != 1 ]; then
  echo "CLOCK_FLOOR_MUTATION_NOT_DETECTED" >&2
  exit 1
fi
echo "DETECTED_CLOCK_FLOOR"

# --- mutation 2: reused pre-roll packet replaced by the current packet -----
build_variant preroll wrong-preroll.patch
preroll_detected=1
pcm_ref_cases=0
for c in "${cases[@]}"; do
  log="$work/preroll-$c.log"
  status=pass
  "$work/preroll/chain-clock" --case "$c" >"$log" 2>&1 || status=fail
  # This mutation keeps every chain's EOS-minus-pre-skip length intact, so
  # all six chain duration checks must pass (CLOCK_OK) and the failure must
  # not come from the length checks.
  if [ "$status" != fail ] || ! grep -qx "CLOCK_OK $c chains=6" "$log"; then
    preroll_detected=0
  fi
  if grep -q "FAIL $c CLOCK_LENGTH" "$log"; then
    preroll_detected=0
  fi
  # Wherever the substitution keeps the container clock valid, the sound
  # reference must be what catches the wrong pre-roll packet.  (In one 60 ms
  # group the substituted packet is the drain-shortened final packet, so the
  # last chain's granule clock itself becomes inconsistent and the granule
  # cross-check reports it instead.)
  if grep -q "FAIL $c PCM_REFERENCE" "$log"; then
    pcm_ref_cases=$((pcm_ref_cases + 1))
  fi
done
if [ "$pcm_ref_cases" -lt 4 ]; then
  preroll_detected=0
fi
if [ "$preroll_detected" != 1 ]; then
  echo "WRONG_PREROLL_MUTATION_NOT_DETECTED" >&2
  exit 1
fi
echo "DETECTED_WRONG_PREROLL"

# --- the run must not have dirtied the production sources ------------------
if [ -n "$(git status --porcelain -- src include devcheck)" ]; then
  echo "PRODUCTION_FILES_DIRTY" >&2
  exit 1
fi
