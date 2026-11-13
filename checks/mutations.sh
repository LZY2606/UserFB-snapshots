#!/bin/bash
# mutations.sh - prove the geometry regression catches genuine source breakage.
#
# Two throw-away copies of the sources each receive exactly one defect:
#   recurse : corrupt the right-half curvature (k1) recursion coefficient in
#             spiro_seg_to_bpath1's subdivision step;
#   offset  : make the cubic output's y translation use the x offset dm[1]
#             instead of dm[2].
# Every copy must still compile and complete a real conversion, and must then
# be rejected by a tangent / distance / coordinate-content assertion. A compile
# failure, crash or timeout is treated as a failure of THIS script, not as a
# valid detection. The pristine working tree is checksummed and never modified.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/libspiro-mut.XXXXXX")"
trap 'rm -rf "$work"' EXIT

timeout_bin="$(command -v timeout || true)"
run_to() {
    local secs="$1"; shift
    if [ -n "$timeout_bin" ]; then "$timeout_bin" "$secs" "$@"; else "$@"; fi
}

# Build one source copy exactly like checks/run.sh does (no autotools needed).
build_copy() { # build_copy <src-dir> <build-dir>
    local src="$1" b="$2"
    mkdir -p "$b/obj"
    local f
    for f in spiro.c bezctx.c spiroentrypoints.c; do
        cc -std=c99 -O2 -I"$src" -I"$src/checks/compat" \
           -c "$src/$f" -o "$b/obj/$f.o"
    done
    c++ -std=c++17 -O2 -I"$src" -I"$src/checks/compat" \
        "$src/checks/target.cpp" "$src/checks/geo.cpp" \
        "$src/checks/driver.cpp" "$b"/obj/*.o -lm -o "$b/target"
}

# A mutated binary is expected to run its conversions and then fail a geometric
# assertion. Crash (signal), timeout (124) or an unexpected failure mode aborts.
expect_geometric_reject() { # <src> <build> <group> <grep-pattern>
    local src="$1" bin="$2" group="$3" pat="$4" log rc
    log="$work/$(basename "$src").log"
    set +e
    ( cd "$src" && run_to 120 "$bin" "$group" ) >"$log" 2>&1
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        echo "ERROR: $group unexpectedly PASSED under mutation" >&2
        cat "$log" >&2; exit 1
    elif [ "$rc" -eq 124 ]; then
        echo "ERROR: $group timed out under mutation" >&2; exit 1
    elif [ "$rc" -gt 128 ]; then
        echo "ERROR: $group crashed (signal $((rc-128))) under mutation" >&2
        cat "$log" >&2; exit 1
    elif ! grep -q "$pat" "$log"; then
        echo "ERROR: $group failed but not via the expected geometric" \
             "assertion /$pat/" >&2
        cat "$log" >&2; exit 1
    fi
    echo "OK  [$group] compiled, converted, then rejected: $(grep "$pat" "$log" | head -1)"
}

sum_before="$(shasum "$root/spiro.c")"

# ---- two disposable copies carrying the same headers and checks tree ----
for name in recurse offset; do
    src="$work/$name"
    mkdir -p "$src"
    cp "$root"/*.c "$root"/*.h "$src"/
    cp -R "$root/checks" "$src/checks"
done

# ---- mutation A: right-half curvature coefficient recursion (.25 -> .5) ----
perl -i -pe 's/ksub\[0\] \+= \.25 \* ks\[1\] \+ \(1\.\/384\) \* ks\[3\];/ksub[0] += .5 * ks[1] + (1.\/384) * ks[3];/' \
     "$work/recurse/spiro.c"
[ "$(grep -c 'ksub\[0\] += .5 \* ks\[1\]' "$work/recurse/spiro.c")" -eq 1 ] || {
    echo "ERROR: mutation A was not applied exactly once" >&2; exit 1; }
[ "$(grep -c 'ksub\[0\] += .25 \* ks\[1\]' "$work/recurse/spiro.c")" -eq 0 ] || {
    echo "ERROR: original recursion line still present in copy A" >&2; exit 1; }

# ---- mutation B: cubic y translation wrongly sourced from x offset dm[1] ----
# Each bezctx_curveto statement spans several physical lines, so use slurp mode
# and scope the replacement to whole curveto statements only. The experimental
# quadratic (bezctx_quadto) path is intentionally left untouched.
perl -0777 -i -pe 's/(bezctx_curveto\(bc.*?;)/ my $z=$1; $z =~ s|dm\[2\]|dm[1]|g; $z /gse' \
     "$work/offset/spiro.c"
# No y-offset (dm[2]) may remain on any cubic curveto statement...
[ "$(grep -A2 bezctx_curveto "$work/offset/spiro.c" | grep -c 'dm\[2\]' || true)" -eq 0 ] || {
    echo "ERROR: mutation B did not replace every cubic y offset" >&2; exit 1; }
# ...while the quadratic path must keep its own dm[2] uses.
[ "$(grep -A3 bezctx_quadto "$work/offset/spiro.c" | grep -c 'dm\[2\]' || true)" -gt 0 ] || {
    echo "ERROR: mutation B leaked into the quadratic path" >&2; exit 1; }
# Exactly the four cubic y-offset sites (two statements x two y arguments) moved.
[ "$(grep -A2 bezctx_curveto "$work/offset/spiro.c" | grep -oE '\(y[01] [+-] v[lr]\) \* dm\[0\] \+ dm\[1\]' | wc -l | tr -d ' ')" -eq 4 ] || {
    echo "ERROR: mutation B cubic y-offset site count unexpected" >&2; exit 1; }

# ---- build both (compile failure here fails this script) ----
build_copy "$work/recurse" "$work/build-recurse"
build_copy "$work/offset"  "$work/build-offset"
echo "Both mutated copies compile and link successfully."

# ---- each must convert valid material, then be rejected by geometry ----
# A corrupts recursive subdivision: the independent integral distance check on
# a recursing k1 segment must fail.
expect_geometric_reject "$work/recurse" "$work/build-recurse/target" \
                        model "bidirectional distance"
# B corrupts only the y offset channel: the distinct x/y translation coordinate
# assertion must fail while x stays exact.
expect_geometric_reject "$work/offset" "$work/build-offset/target" \
                        normalization "translation channel"

# ---- pristine tree must be untouched ----
sum_after="$(shasum "$root/spiro.c")"
[ "$sum_before" = "$sum_after" ] || {
    echo "ERROR: pristine spiro.c was modified by the mutation run" >&2; exit 1; }
echo "Pristine sources unchanged ($sum_after)."
echo "ALL MUTATION CHECKS PASSED: each defect compiled, converted, and was caught."
