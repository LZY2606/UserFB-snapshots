#!/usr/bin/env bash
#
# regress/check_fill.sh
#
# Builds the real PlutoVG library in a temporary directory and runs the
# independent fill reference suite.  With --mutations it additionally builds
# three isolated, deliberately broken source copies and requires the same
# suite to reject each one through a numeric assertion failure.
#
# No Python is used anywhere; the only image output is a binary PPM written
# by the C test itself.  The product working tree is never modified.
set -euo pipefail

mode="${1:-normal}"
case "$mode" in
    normal|--mutations) ;;
    *) echo "usage: $0 [--mutations]" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "$0")/.." && pwd)"
base_tmp="$(mktemp -d "${TMPDIR:-/tmp}/fillref.XXXXXX")"
trap 'rm -rf "$base_tmp"' EXIT

build="$base_tmp/build"
mkdir -p "$build"
cc_bin="${CC:-cc}"
common_flags=(-std=c11 -O1 -DPLUTOVG_BUILD_STATIC
    "-I$root/include" "-I$root/regress")

run_with_timeout() {
    # Portable timeout: perl alarm + exec, works without GNU coreutils.
    local secs="$1"; shift
    perl -e 'alarm shift; exec @ARGV' "$secs" "$@"
}

# Compile a test binary against a chosen source directory.
#   $1 output binary, $2 source dir, $3.. extra C files (the test itself)
compile_suite() {
    local out="$1"; local srcdir="$2"; shift 2
    "$cc_bin" "${common_flags[@]}" "-I$srcdir" "$@" "$srcdir"/*.c -lm -o "$out"
}

# Returns 0 and prints "cases=N asserts=M" when the binary really ran a
# non-empty suite; fails the whole script on zero cases / timeouts.
inspect_run() {
    local log="$1"
    local cases asserts
    cases="$(sed -n 's/^FILL_REFERENCE_CASES=//p' "$log" | tail -n1)"
    asserts="$(sed -n 's/^FILL_REFERENCE_ASSERTS=//p' "$log" | tail -n1)"
    if [[ -z "$cases" || "$cases" -le 0 ]]; then
        echo "ERROR: suite reported zero scenarios ($log)" >&2
        exit 1
    fi
    if [[ -z "$asserts" || "$asserts" -le 0 ]]; then
        echo "ERROR: suite reported zero assertions ($log)" >&2
        exit 1
    fi
    printf 'cases=%s asserts=%s\n' "$cases" "$asserts"
}

echo "==> building fill reference suite against the product sources"
suite="$build/fill_reference"
compile_suite "$suite" "$root/source" \
    "$root/regress/fill_reference.c" \
    "$root/regress/ref_geometry.c" \
    "$root/regress/ppm_image.c"

# The pre-existing devcheck baseline must keep building and passing.
echo "==> rebuilding the original devcheck baseline"
"$cc_bin" -std=c11 -O1 -DPLUTOVG_BUILD_STATIC \
    "-I$root/include" "-I$root/source" \
    "$root/devcheck/baseline.c" "$root/source"/*.c -lm \
    -o "$build/baseline"
"$build/baseline"

outdir="$base_tmp/ppm"
mkdir -p "$outdir"
echo "==> running fill reference suite"
set +e
run_with_timeout 180 "$suite" "$outdir" >"$build/suite.stdout" 2>"$build/suite.stderr"
suite_status=$?
set -e
cat "$build/suite.stdout"
if [[ $suite_status -ne 0 ]]; then
    cat "$build/suite.stderr" >&2
    echo "ERROR: fill reference suite failed" >&2
    exit 1
fi
inspect_run "$build/suite.stdout" >/dev/null
grep -q '^FILL_REFERENCE_OK$' "$build/suite.stdout"

# Keep one diagnostic image outside the auto-cleaned temp tree.
kept_dir="$root/regress/output"
mkdir -p "$kept_dir"
cp "$outdir/cubic_arch_nonzero.ppm" "$kept_dir/fill_reference_triptych.ppm"
echo "PPM_DIAGNOSTIC=$kept_dir/fill_reference_triptych.ppm"
grep -E '^FILL_REFERENCE_(CASES|ASSERTS|MAX_CURVE_BAND|LINE_STABILITY)=' \
    "$build/suite.stdout"
echo "FILL_REFERENCE_OK"

if [[ "$mode" != "--mutations" ]]; then
    exit 0
fi

# ------------------------------------------------------------------
# Mutation testing: three isolated broken source copies
# ------------------------------------------------------------------

apply_rule_mutation() {
    # Make even-odd fills take the non-zero (NONE flags) path.
    perl -0pi -e 's/case PLUTOVG_FILL_RULE_EVEN_ODD:\s*\n\s*outline->flags = PVG_FT_OUTLINE_EVEN_ODD_FILL;/case PLUTOVG_FILL_RULE_EVEN_ODD:\n            outline->flags = PVG_FT_OUTLINE_NONE;/' \
        "$1/plutovg-rasterize.c"
}

apply_cubic_mutation() {
    # Degrade every cubic Bezier to the straight chord between its endpoints.
    perl -0pi -e 's/(gray_render_cubic\( RAS_ARG_ const PVG_FT_Vector\*  control1,\s*\n\s*const PVG_FT_Vector\*  control2,\s*\n\s*const PVG_FT_Vector\*  to \)\s*\n\s*\{)/$1\n    gray_line_to( RAS_VAR_ UPSCALE(to->x), UPSCALE(to->y) );\n    return;/' \
        "$1/plutovg-ft-raster.c"
}

apply_clip_mutation() {
    # A second clip layer replaces the stored region instead of intersecting.
    perl -0pi -e 's/plutovg_span_buffer_intersect\(&canvas->clip_spans, &canvas->fill_spans, &canvas->state->clip_spans\);\s*\n\s*plutovg_span_buffer_copy\(&canvas->state->clip_spans, &canvas->clip_spans\);/plutovg_span_buffer_copy(\&canvas->state->clip_spans, \&canvas->fill_spans);/' \
        "$1/plutovg-canvas.c"
}

run_mutation() {
    local tag="$1"; local marker="$2"; local mutator="$3"
    local work="$base_tmp/$tag"
    local src="$work/source"
    mkdir -p "$work"
    cp -R "$root/source" "$src"
    "$mutator" "$src"

    local bin="$work/check"
    echo "==> mutation $tag: compiling isolated source copy"
    if ! compile_suite "$bin" "$src" \
        "$root/regress/fill_reference.c" \
        "$root/regress/ref_geometry.c" \
        "$root/regress/ppm_image.c" 2>"$work/build.err"; then
        cat "$work/build.err" >&2
        echo "ERROR: mutation $tag failed to compile (not a detection)" >&2
        exit 1
    fi

    local mod_out="$work/ppm"
    mkdir -p "$mod_out"
    echo "==> mutation $tag: running suite (numeric failure required)"
    set +e
    run_with_timeout 180 "$bin" "$mod_out" >"$work/run.stdout" 2>"$work/run.stderr"
    local st=$?
    set -e
    if [[ $st -eq 124 || $st -eq 142 ]]; then
        echo "ERROR: mutation $tag timed out" >&2
        exit 1
    fi
    if ! inspect_run "$work/run.stdout" >"$work/run.summary"; then
        exit 1
    fi
    if [[ $st -eq 0 ]]; then
        echo "ERROR: mutation $tag was not detected" >&2
        exit 1
    fi
    if ! grep -q '^FILL_REFERENCE_FAILURE ' "$work/run.stderr"; then
        echo "ERROR: mutation $tag failed without a numeric assertion record" >&2
        cat "$work/run.stderr" >&2
        exit 1
    fi
    head -n1 "$work/run.stderr"
    cat "$work/run.summary"
    echo "$marker"
}

run_mutation rule MUTATION_RULE_REJECTED apply_rule_mutation
run_mutation cubic MUTATION_CUBIC_REJECTED apply_cubic_mutation
run_mutation clip MUTATION_CLIP_REJECTED apply_clip_mutation

echo "MUTATIONS_ALL_REJECTED"
