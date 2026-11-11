#!/usr/bin/env bash
#
# Numeric regression contract for two-point conical (radial) gradients.
#
#   bash harness/check-radial.sh
#
# What it does, in order:
#   1. Builds and runs harness/radial_contract.rs against the real library,
#      proving every contract case actually executes on both the default and
#      force-HQ pipelines.
#   2. Runs the three pre-existing PNG reference tests
#      (well_behaved_radial, conical_smaller_radial, simple_radial_with_ts_hq),
#      locating their fixtures the same way harness/current.sh does.
#   3. Builds two *independent* temporary source copies, applies one focused
#      mutation per copy and proves the same contract fails:
#        A. ignore_shader_transform.patch - gradient coordinates ignore the
#           shader's own transform (the path transform is left untouched);
#        B. keep_invalid_conical_mask.patch - invalid two-circle solutions no
#           longer clear the conical validity mask.
#      A compile error, a panic before rendering or zero executed cases are
#      explicitly rejected as detections. Per-pixel reports (case, coordinate,
#      geometry validity, expected vs actual bytes) are saved to a report dir.
#
# All dependencies stay pinned by harness/Cargo.toml + harness/Cargo.lock and
# every build target lives in a temporary directory. No production source and
# no reference PNG is modified or regenerated.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
export CARGO_HOME="$root/harness/.cargo-home"
export TMPDIR="${TMPDIR:-/tmp}"

work="$(mktemp -d -p "$TMPDIR")"
reports="$TMPDIR/radial-contract-reports-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$reports"
cleanup() {
    rm -rf "$work"
    rm -rf "$root/harness/tests"
}
trap cleanup EXIT

manifest="$root/harness/Cargo.toml"
expected_cases=12 # 6 geometries x (default pipeline + forced HQ pipeline)

fail() {
    echo "RADIAL_CONTRACT_SCRIPT_ERROR: $*" >&2
    exit 1
}

echo "== [1/3] numeric radial contract on the real library =="
main_log="$reports/00-real-library.log"
cargo run --locked --manifest-path "$manifest" --target-dir "$work/build-main" \
    --bin radial_contract 2>&1 | tee "$main_log"

grep -q '^RADIAL_CONTRACT_PASS ' "$main_log" \
    || fail "contract did not pass on the unmodified library"
ran="$(grep -c '^CASE_DONE ' "$main_log")"
[[ "$ran" -eq "$expected_cases" ]] \
    || fail "expected $expected_cases executed case-runs, found $ran"
# Material invariants printed by the binary: out-of-range samples for all three
# spread modes, invalid geometry, outside-contour pixels and SIMD tail pixels.
grep -q '^RADIAL_OUT_OF_RANGE pad=[1-9].* repeat=[1-9].* reflect=[1-9]' "$main_log" \
    || fail "not every spread mode sampled outside 0..1"
grep -Eq '^RADIAL_GEOMETRY valid=[1-9][0-9]* invalid=[1-9][0-9]* .*outside_contour=[1-9][0-9]* simd_tail_pixels=[1-9][0-9]*' "$main_log" \
    || fail "contract material invariants (invalid/outside/tail pixels) missing"

echo
echo "== [2/3] pre-existing PNG reference tests =="
# Same fixture location strategy as harness/current.sh.
mkdir -p "$root/harness/tests/images/gradients"
for fixture in well-behaved-radial conical-smaller-radial simple-radial-with-ts-hq; do
    cp "$root/tests/images/gradients/$fixture.png" \
       "$root/harness/tests/images/gradients/$fixture.png"
done
for name in well_behaved_radial conical_smaller_radial simple_radial_with_ts_hq; do
    out="$(cargo test --locked --manifest-path "$manifest" \
        --target-dir "$work/build-old" --test old_radial "gradients::$name" \
        -- --exact --nocapture)"
    printf '%s\n' "$out"
    grep -F "test gradients::$name ... ok" <<<"$out" \
        || fail "legacy PNG test $name did not pass"
done

# Copy the library + harness into an isolated temp dir, without build caches.
# The repo's .git is a relative symlink that does not survive the copy, so the
# mutation is applied with plain `patch -p1` rather than git.
make_copy() {
    local dest="$1"
    rsync -a \
        --exclude '/debug' \
        --exclude '/scratch-radial' \
        --exclude '/target' \
        --exclude 'target' \
        --exclude '.cargo-home' \
        --exclude '/harness/tests' \
        "$root/" "$dest/"
}

# Run one mutation copy and validate that the failure is a genuine pixel-level
# detection rather than a build break, an early panic or an empty run.
check_mutation() {
    local tag="$1" patch="$2" log="$3" required_geometry="$4" marker="$5"
    local dest="$work/copy-$tag"
    make_copy "$dest"
    ( cd "$dest" && patch -p1 --dry-run --silent < "harness/mutations/$patch" ) \
        || fail "mutation $tag patch does not apply"
    ( cd "$dest" && patch -p1 --silent < "harness/mutations/$patch" )

    set +e
    cargo run --locked --manifest-path "$dest/harness/Cargo.toml" \
        --target-dir "$work/build-$tag" --bin radial_contract \
        >"$log" 2>&1
    local rc=$?
    set -e
    cat "$log"

    # The binary must have built, run all cases and then failed on assertions.
    [[ "$rc" -ne 0 ]] || fail "mutation $tag was not detected (contract passed)"
    grep -q 'panicked at' "$log" \
        && fail "mutation $tag panicked before/while rendering"
    local ran_mut
    ran_mut="$(grep -c '^CASE_DONE ' "$log" || true)"
    [[ "$ran_mut" -eq "$expected_cases" ]] \
        || fail "mutation $tag ran only $ran_mut of $expected_cases case-runs"
    grep -q '^RADIAL_CASES_EXECUTED ' "$log" \
        || fail "mutation $tag produced no executed-case evidence"

    # The untouched concentric identity control must remain green in both
    # pipelines; the planted change is not allowed to disturb it.
    local control_bad
    control_bad="$(grep '^CASE_DONE name=control_concentric_identity ' "$log" \
        | grep -vc 'failures=0' || true)"
    [[ "$control_bad" -eq 0 ]] \
        || fail "mutation $tag disturbed the identity control case"

    # The detection must be a concrete pixel assertion carrying case, pixel,
    # geometry validity and expected/actual bytes, on the intended geometry.
    grep -Eq "^ASSERT_FAIL .* geometry=$required_geometry( |\$).*expected.*actual=" "$log" \
        || fail "mutation $tag produced no $required_geometry pixel assertion"

    echo "$marker"
}

echo
echo "== [3/3] independent mutated source copies must be detected =="
check_mutation "a" "ignore_shader_transform.patch" \
    "$reports/10-mutation-ignore-shader-transform.log" \
    "Valid" "DETECTED_SHADER_TRANSFORM"
check_mutation "b" "keep_invalid_conical_mask.patch" \
    "$reports/20-mutation-keep-invalid-conical-mask.log" \
    "Invalid" "DETECTED_CONICAL_MASK"

echo
echo "Per-pixel detection reports saved in: $reports"
echo "RADIAL_CONTRACT_OK"
