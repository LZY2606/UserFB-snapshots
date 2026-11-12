#!/usr/bin/env bash
#
# Mutation-based check of the numeric filter contract.
#
# For each of three numerical degradations a throwaway copy of the source
# tree is made, the mutation is applied, the static library and the contract
# test are rebuilt, and the corresponding contract group must reject the
# mutated library with a *numeric* failure.  Compile failures, library errors
# and empty output are treated as failures of this script, not as detections.
#
# The working tree is never modified: all copies and build trees live in a
# temporary directory that is removed on exit.

set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/soxr-mut.XXXXXX")
trap 'rm -rf "$work"' EXIT

groups=(passband alias phase)

self_fail() {
  echo "CHECK-MUTATIONS SELF-FAILURE: $*" >&2
  exit 1
}

cmake_args=(
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  -DBUILD_SHARED_LIBS=OFF
  -DBUILD_TESTS=OFF
  -DBUILD_EXAMPLES=OFF
  -DWITH_LSR_BINDINGS=OFF
  -DWITH_OPENMP=OFF
  -DWITH_CR32S=OFF
  -DWITH_CR64S=OFF
  -Wno-dev
)

# Build libsoxr from $1 (source tree) into $2 (build tree) and link the
# contract test against the resulting static library.
build_contract() {
  local src=$1 build=$2
  cmake -S "$src" -B "$build" "${cmake_args[@]}" >/dev/null \
    || self_fail "cmake configure failed for $src"
  cmake --build "$build" --target soxr -j2 >/dev/null \
    || self_fail "compile failed for $src"
  test -s "$build/src/libsoxr.a" \
    || self_fail "static library missing for $src"
  cc -std=c99 -Wall -Wextra -I "$src/src" \
     "$root/regress/filter_contract.c" "$build/src/libsoxr.a" -lm \
     -o "$build/filter-contract" \
    || self_fail "contract test compile failed for $src"
}

# Run one contract group; returns 0 on PASS, 1 on numeric FAIL.  Any setup
# failure (ERROR line, empty output) aborts the whole script.
run_group() {
  local binary=$1 group=$2 out
  out=$("$binary" "$group" 2>&1) || true
  test -n "$out" || self_fail "$group produced no output from $binary"
  if grep -q '^ERROR ' <<<"$out"; then
    self_fail "$group hit a setup/contract error (not a numeric deviation): $out"
  fi
  printf '%s\n' "$out"
  grep -Eq "^PASS $group checks=[1-9][0-9]*$" <<<"$out"
}

# A mutation is detected only when the designated group fails numerically.
expect_detection() {
  local binary=$1 group=$2 name=$3 out
  out=$("$binary" "$group" 2>&1) || true
  test -n "$out" || self_fail "$name: $group gave empty output"
  if grep -q '^ERROR ' <<<"$out"; then
    self_fail "$name: library/setup error instead of numeric detection: $out"
  fi
  grep -q "^FAIL $group " <<<"$out" \
    || self_fail "$name: $group did not detect the mutation: $out"
  grep -Eq 'observed=[^ ]+ tolerance=[^ ]+' <<<"$out" \
    || self_fail "$name: failure line lacks observed/tolerance evidence: $out"
  echo "detected $name via $group: $out"
}

echo '== baseline library: all three groups must pass =='
build_contract "$root" "$work/base-build"
base_bin="$work/base-build/filter-contract"
for g in "${groups[@]}"; do
  run_group "$base_bin" "$g" >/dev/null || self_fail "baseline failed $g"
done
echo 'baseline: passband alias phase all PASS'

# Each entry: name, designated detecting group, sed expression.
mutations=(
  'gain passband s#double mult = scale / lsx_bessel_I_0(beta)#double mult = (0.99 * scale) / lsx_bessel_I_0(beta)#'
  'attenuation alias s#double att = (bits1 + 1) \* linear_to_dB(2.), attArb = att;#double att = 24., attArb = att;#'
  'phase phase s#double const phase_response = q_spec->phase_response, tbw0#double const phase_response = 50, tbw0#'
)

for entry in "${mutations[@]}"; do
  name=${entry%% *}
  rest=${entry#* }
  group=${rest%% *}
  expr=${rest#* }

  src="$work/$name-src"
  build="$work/$name-build"
  cp -R "$root" "$src" || self_fail "cannot copy source tree for $name"

  case $name in
    gain)       target=$src/src/filter.c ;;
    attenuation|phase) target=$src/src/cr.c ;;
  esac
  sed -e "$expr" "$target" > "$target.tmp" || self_fail "sed failed for $name"
  mv "$target.tmp" "$target"
  grep -q '0.99 \* scale' "$target" 2>/dev/null || \
  grep -q 'double att = 24\.' "$target" 2>/dev/null || \
  grep -q 'phase_response = 50, tbw0' "$target" 2>/dev/null || \
    self_fail "mutation $name was not applied"
  echo "== mutation $name applied; building =="

  build_contract "$src" "$build"
  bin="$build/filter-contract"

  # Every group must run to completion (numeric pass or numeric fail only).
  for g in "${groups[@]}"; do
    run_group "$bin" "$g" >/dev/null || true
  done

  expect_detection "$bin" "$group" "$name"
done

echo 'CHECK-MUTATIONS OK: 3/3 mutations detected by their designated groups'
