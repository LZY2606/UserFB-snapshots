#!/usr/bin/env bash
#
# Mutation probe for tests/wsola_reference.cpp.
#
# Proves the independent regression actually catches two degradations that
# still compile and run, by building each one from a TEMPORARY COPY of
# TDStretch.cpp. The normal library (.build/libsoundtouch.a) and the real
# source tree are never modified; every mutated artifact lives under
# .task-build/mutation and is disposable.
#
# Degradation A ("no-norm"):
#   Drop the candidate-energy normalization from the floating-point
#   calcCrossCorr / calcCrossCorrAccumulate return values. A correct
#   regression must then fail on a window choice or rebuilt PCM mismatch.
#
# Degradation B ("reset-fraction"):
#   Zero the nominalSkip fractional residue after every processing round.
#   A correct regression must then fail on the consumption/skip history (and
#   consequently the window choices/PCM at fractional tempos).
#
# The baseline library must keep passing, each mutant must exit non-zero with
# a FIRST_DIFF line naming the earliest concrete divergence, and failures must
# be ordinary assertion exits rather than build errors or signal aborts.
#
set -euo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-c++}
COMMON_FLAGS=(-std=c++17 -O2 -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS
              -I. -Iinclude -Isource/SoundTouch -I.build)
WORK=.task-build/mutation
mkdir -p "$WORK/obj"

log() { printf 'MUTATION %s\n' "$*"; }

# Build a mutated static library from a transformed copy of TDStretch.cpp.
# $1 = variant name, $2 = perl substitution, $3 = expected replacement count
build_mutated_library() {
    local name=$1 substitution=$2 expected_count=$3
    local dir="$WORK/$name"
    mkdir -p "$dir/obj"

    # Recompile every translation unit fresh; only TDStretch.cpp is altered.
    local file object
    for file in source/SoundTouch/*.cpp; do
        object="$dir/obj/$(basename "${file%.cpp}").o"
        if [[ $(basename "$file") == TDStretch.cpp ]]; then
            cp "$file" "$dir/TDStretch.cpp"
            local actual
            actual=$(perl -ne 'BEGIN{$n=0} if ('"$substitution"'){$n++} END{print $n}' "$dir/TDStretch.cpp")
            if [[ "$actual" != "$expected_count" ]]; then
                echo "mutation $name: expected $expected_count substitutions, found $actual" >&2
                exit 2
            fi
            perl -i -pe "$substitution" "$dir/TDStretch.cpp"
            "$CXX" "${COMMON_FLAGS[@]}" -c "$dir/TDStretch.cpp" -o "$object"
        else
            "$CXX" "${COMMON_FLAGS[@]}" -c "$file" -o "$object"
        fi
    done
    ar rcs "$dir/libsoundtouch_mutated.a" "$dir"/obj/*.o
}

# Compile the unchanged regression + driver against a chosen library.
build_target() {
    local library=$1 output=$2
    "$CXX" "${COMMON_FLAGS[@]}" -x c++ task_support/driver.c tests/wsola_reference.cpp \
        -x none "$library" -o "$output"
}

# Run a target binary; $1 = path, $2 = expect ("pass" or "fail").
run_expectation() {
    local binary=$1 expectation=$2
    local logfile="$binary.log"
    set +e
    "$binary" >"$logfile" 2>&1
    local status=$?
    set -e

    if [[ $expectation == pass ]]; then
        if [[ $status != 0 ]]; then
            cat "$logfile" >&2
            echo "expected $binary to pass, exit=$status" >&2
            exit 3
        fi
        grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$logfile"
        log "baseline pass confirmed ($binary)"
        return
    fi

    # Failure must be an ordinary assertion exit (driver returns 1..6), never a
    # crash signal (>=128) and never a build failure (which would not run at all).
    if (( status < 1 || status > 127 )); then
        cat "$logfile" >&2
        echo "mutant $binary exited $status, expected a non-signal assertion failure" >&2
        exit 4
    fi
    # The log must name the earliest concrete content divergence.
    if ! grep -q 'FIRST_DIFF' "$logfile"; then
        cat "$logfile" >&2
        echo "mutant $binary failed without a FIRST_DIFF annotation" >&2
        exit 5
    fi
    log "detected $binary exit=$status first_diff=$(grep -m1 'FIRST_DIFF' "$logfile" | sed 's/.*FIRST_DIFF //')"
}

# 1. Baseline: the unmodified library must keep passing.
bash task_support/build.sh >/dev/null
build_target .build/libsoundtouch.a "$WORK/target_baseline"
run_expectation "$WORK/target_baseline" pass

# 2. Degradation A: remove candidate-energy normalization. The pattern matches
#    only the two floating-point sites; the integer routines use different text
#    ((double)corr / sqrt((norm < 1e-9) ? ...)) and are left untouched.
build_mutated_library \
    no-norm \
    's{corr / sqrt\(\(norm < 1e-9 \? 1\.0 : norm\)\)}{corr}g' \
    2
build_target "$WORK/no-norm/libsoundtouch_mutated.a" "$WORK/target_no_norm"
run_expectation "$WORK/target_no_norm" fail

# 3. Degradation B: discard the fractional nominalSkip residue each round
#    (single site in processSamples).
build_mutated_library \
    reset-fraction \
    's{skipFract -= ovlSkip;}{skipFract -= ovlSkip; skipFract = 0.0f;}g' \
    1
build_target "$WORK/reset-fraction/libsoundtouch_mutated.a" "$WORK/target_reset_fraction"
run_expectation "$WORK/target_reset_fraction" fail

# 4. Prove the normal library is untouched: baseline still passes.
build_target .build/libsoundtouch.a "$WORK/target_baseline_recheck"
run_expectation "$WORK/target_baseline_recheck" pass

log "PROBE_OK mutants=2 detected=2 baseline_intact=1"
