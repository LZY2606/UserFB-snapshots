#!/bin/bash
# Scalar vs NEON timing harness for the AArch64 back end.
#
# Builds support/bench.c together with the library sources using the same
# compiler and the same -O3 for both arms (no flags that would disable the
# scalar auto-vectoriser), then runs the three workloads.
#
#   bash support/bench.sh [stream]
#
# The stream argument defaults to tests/neon_move.m2v.  No pass/fail
# millisecond threshold is applied; the script only reports.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
stream=${1:-$repo/tests/neon_move.m2v}

if [[ ! -f $stream ]]; then
    echo "bench: missing stream $stream" >&2
    exit 2
fi

# mktemp's default template lands in a directory this sandbox does not
# allow; use an explicit, writable location instead.
build=${MPEG2_NEON_BENCH_BUILD:-$(mktemp -d "${TMPDIR:-/tmp}/mpeg2neon.XXXXXXXXXX")}
cleanup=
if [[ -z ${MPEG2_NEON_BENCH_BUILD:-} ]]; then
    cleanup=$build
fi
trap 'if [[ -n $cleanup ]]; then rm -rf "$cleanup"; fi' EXIT

mkdir -p "$build"
printf '#define HAVE_BUILTIN_EXPECT 1\n#define ATTRIBUTE_ALIGNED_MAX 16\n#if defined(__aarch64__)\n#define ARCH_AARCH64 1\n#endif\n' > "$build/config.h"

src=(
    libmpeg2/alloc.c libmpeg2/header.c libmpeg2/decode.c libmpeg2/slice.c
    libmpeg2/motion_comp.c libmpeg2/idct.c libmpeg2/cpu_accel.c
    libmpeg2/cpu_state.c libmpeg2/idct_neon.c libmpeg2/motion_comp_neon.c
)

for f in libmpeg2/*_neon.c; do
    [[ -f $f ]] || continue
    case " ${src[*]} " in
    *" $f "*) ;;
    *) src+=("$f") ;;
    esac
done

cc -O3 -std=c99 -I"$build" -I"$repo/include" -I"$repo/libmpeg2" \
    "${src[@]/#/$repo/}" "$repo/support/bench.c" -o "$build/bench"

echo "=== scalar vs NEON, same compiler, -O3 ==="
"$build/bench" "$stream"
