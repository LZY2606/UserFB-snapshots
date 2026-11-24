#!/bin/bash
# Isolated source-mutation checks for tests/field_prediction_cases.c.
# The normal regression binary is rebuilt in each copied tree.  A valid
# detection means: the mutated production code compiles and runs, and the
# public-API content oracle reaches a concrete pixel mismatch (driver exit 4).
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/.task-build/mutants"
rm -rf "$WORK"
mkdir -p "$WORK"

make_config() {
    local d=$1
    mkdir -p "$d/cfg" "$d/obj"
    printf '#define HAVE_BUILTIN_EXPECT 1\n#define ATTRIBUTE_ALIGNED_MAX 16\n' \
        > "$d/cfg/config.h"
}

build_mutant() {
    local d=$1
    local cc=${CC:-cc}
    local src
    for src in alloc header decode slice motion_comp idct cpu_accel cpu_state; do
        "$cc" -O2 -std=c99 -I"$d/cfg" -I"$d" -I"$d/include" \
            -I"$d/libmpeg2" -c "$d/libmpeg2/$src.c" \
            -o "$d/obj/$src.o" || return 1
    done
    "$cc" -O2 -std=c99 -I"$d/cfg" -I"$d" -I"$d/include" \
        -I"$d/libmpeg2" \
        "$d/task_support/driver.c" "$d/tests/field_prediction_cases.c" \
        "$d/obj/"*.o -lm -o "$d/target" || return 1
    return 0
}

run_mutant() {
    local name=$1
    local patch=$2
    local d="$WORK/$name"
    mkdir -p "$d"
    cp -R "$ROOT/libmpeg2" "$ROOT/include" "$ROOT/task_support" "$ROOT/tests" "$d/"
    make_config "$d"

    if ! perl -0777 -pi -e "$patch" "$d/libmpeg2/slice.c"; then
        echo "MUTATION_PATCH_FAIL $name"
        return 1
    fi
    if ! build_mutant "$d"; then
        echo "MUTATION_BUILD_FAIL $name"
        return 2
    fi

    (
        cd "$d"
        FIELD_PRED_MUTATION_CHILD=1 "$d/target" >"$d/run.log" 2>&1
    ) &
    local pid=$!
    (
        sleep 30
        kill -9 "$pid" 2>/dev/null || true
    ) &
    local killer=$!
    wait "$pid"
    local status=$?
    kill "$killer" 2>/dev/null || true
    wait "$killer" 2>/dev/null || true

    if [ "$status" -ne 4 ]; then
        echo "MUTATION_RUN_STATUS_BAD $name status=$status"
        sed -n '1,80p' "$d/run.log"
        return 3
    fi
    if ! grep -q 'PIXEL_DIFF picture=' "$d/run.log"; then
        echo "MUTATION_NO_PIXEL_DIFF $name"
        sed -n '1,120p' "$d/run.log"
        return 4
    fi
    if ! grep -Eq 'x=[0-9]+ .*(y=|field_row=)' "$d/run.log"; then
        echo "MUTATION_NO_COORDINATE $name"
        sed -n '1,120p' "$d/run.log"
        return 5
    fi
    echo "MUTATION_DETECTED $name"
    grep -m2 'PIXEL_DIFF picture=' "$d/run.log" || true
    return 0
}

rc=0
# Exchange the half and three-half temporal scalings for the two field
# destinations in frame-picture dual prime.  Match the complete ordered pair
# so the replacement cannot match itself.
patch_coeff='s/m = decoder->top_field_first \? 1 : 3;(.*?)m = decoder->top_field_first \? 3 : 1;/m = decoder->top_field_first ? 3 : 1;$1m = decoder->top_field_first ? 1 : 3;/s'
run_mutant swap_frame_dual_coefficients "$patch_coeff" || rc=10

# Disable use of the current reconstructed first field as the opposite field
# reference for a second non-B field picture.
patch_current='s/if \(decoder->second_field && \(decoder->coding_type != B_TYPE\)\)/if (0 \&\& decoder->second_field)/'
run_mutant forbid_current_first_field "$patch_current" || rc=11

exit "$rc"
