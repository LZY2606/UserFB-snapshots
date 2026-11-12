#!/bin/sh
# checks/check-deformation-mutants.sh
#
# Mutation regression for the independent deformation reference.
# Three separate temporary copies of ufbx.c are built, each with one targeted
# mutation applied from checks/patches:
#   m1_antipode   - DQ antipodal quaternion sign correction removed
#   m2_blendflip  - per-vertex DQ/LBS blend weights swapped
#   m3_order      - blend shape offsets added after skinning
#
# The unmutated library must pass first. Every mutant must compile, load the
# scenes and then FAIL on a position/normal numerical assertion (exit code 2
# with MUTANT_CAPTURE lines). Compile failures, missing patches, load failures
# and crashes are reported as distinct errors and never count as a caught
# deformation defect.
#
# Usage: sh checks/check-deformation-mutants.sh [output-dir]
set -u

# Resolve the repository root from this script's location.
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/.." && pwd)
cd "$root"

out_dir=${1:-$(mktemp -d "${TMPDIR:-/tmp}/deform_mutants.XXXXXX")}
work=$(mktemp -d "${TMPDIR:-/tmp}/deform_mutants_work.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$out_dir"
echo "mutant evidence directory: $out_dir"

cc_bin=${CC:-cc}
cxx_bin=${CXX:-c++}
cflags="-std=c99 -O2 -I."
cxxflags="-std=c++11 -O2 -I."

error() {
	echo "MUTANT_SCRIPT_ERROR: $*" >&2
	exit 1
}

# --- Baseline: the real library must pass the full reference first ---------
"$cc_bin" $cflags -c ufbx.c -o "$work/ufbx_base.o" || error "baseline ufbx.c compile failed"
"$cxx_bin" $cxxflags checks/deformation_reference.cpp "$work/ufbx_base.o" -lm -o "$work/test_base" \
	|| error "baseline reference link failed"
"$work/test_base" "$work" > "$work/base.out" 2>&1
base_code=$?
if [ "$base_code" -ne 0 ]; then
	cat "$work/base.out" >&2
	error "baseline library failed the reference (exit $base_code); mutants cannot be judged"
fi
for mark in UFBX_DQ_REFERENCE_OK UFBX_BLEND_SKIN_ORDER_OK \
            UFBX_DEFORMED_NORMALS_OK UFBX_SCENE_REFERENCE_OK; do
	grep -Fx "$mark" "$work/base.out" >/dev/null || error "baseline missing marker $mark"
done
echo "baseline: UFBX library passes the independent deformation reference"

# --- One entry per mutant: name and patch file ------------------------------
set -- \
	"m1_antipode checks/patches/m1_antipode.patch" \
	"m2_blendflip checks/patches/m2_blendflip.patch" \
	"m3_order checks/patches/m3_order.patch"

captured_total=0
for entry in "$@"; do
	name=${entry%% *}
	patch_file=${entry#* }
	dir="$work/$name"
	mkdir -p "$dir"

	echo "--- mutant $name ($patch_file) ---"

	# Missing patch is its own failure class.
	[ -s "$patch_file" ] || error "$name: patch not found: $patch_file"

	# Independent source copy, patched in isolation.
	cp ufbx.c "$dir/ufbx.c"
	if ! (cd "$dir" && patch -p1 -s < "$root/$patch_file"); then
		error "$name: patch failed to apply"
	fi
	grep -q "MUTANT:" "$dir/ufbx.c" || error "$name: patch applied but mutation marker missing"

	# Compile failures are reported separately, never treated as a catch.
	if ! "$cc_bin" $cflags -c "$dir/ufbx.c" -o "$dir/ufbx.o" 2> "$dir/cc.err"; then
		cat "$dir/cc.err" >&2
		error "$name: mutated ufbx.c failed to compile"
	fi
	if ! "$cxx_bin" $cxxflags checks/deformation_reference.cpp "$dir/ufbx.o" -lm -o "$dir/test" 2>> "$dir/cc.err"; then
		cat "$dir/cc.err" >&2
		error "$name: reference failed to link against the mutant"
	fi

	# Run the full regression. A deformation defect means exit code 2 with
	# concrete MUTANT_CAPTURE evidence; anything else is a harness failure.
	"$dir/test" "$dir" > "$dir/out" 2>&1
	code=$?

	if grep -q "LOAD_FAILED" "$dir/out"; then
		cat "$dir/out" >&2
		error "$name: scene loading failed; no deformation verdict possible"
	fi
	if [ "$code" -gt 128 ]; then
		cat "$dir/out" >&2
		error "$name: crashed (signal $((code-128))); crashes do not count as deformation catches"
	fi
	if [ "$code" -eq 0 ]; then
		cat "$dir/out" >&2
		error "$name: unexpectedly passed; mutation was not detected"
	fi
	if [ "$code" -ne 2 ]; then
		cat "$dir/out" >&2
		error "$name: failed with unclassified exit $code (expected 2)"
	fi

	capture_lines=$(grep -c "MUTANT_CAPTURE" "$dir/out" || true)
	if [ "$capture_lines" -lt 1 ]; then
		cat "$dir/out" >&2
		error "$name: failed without any MUTANT_CAPTURE evidence"
	fi
	pos_captures=$(grep -c "MUTANT_CAPTURE kind=position" "$dir/out" || true)
	nrm_captures=$(grep -c "MUTANT_CAPTURE kind=normal" "$dir/out" || true)
	if [ "$pos_captures" -lt 1 ] && [ "$nrm_captures" -lt 1 ]; then
		cat "$dir/out" >&2
		error "$name: no position/normal assertion capture found"
	fi

	# Persist the per-vertex / per-corner evidence (id, time/weight, expected
	# and actual values) next to deformation.obj for inspection.
	evidence="$out_dir/mutant_${name}_captures.txt"
	{
		echo "# mutant=$name patch=$patch_file exit=$code captures=$capture_lines (position=$pos_captures normal=$nrm_captures)"
		grep "MUTANT_CAPTURE" "$dir/out"
	} > "$evidence"
	captured_total=$((captured_total + capture_lines))
	echo "$name: caught by $capture_lines assertion(s) [position=$pos_captures normal=$nrm_captures]; evidence: $evidence"
	grep "MUTANT_CAPTURE" "$dir/out" | head -2
done

if [ "$captured_total" -lt 1 ]; then
	error "no mutant produced deformation evidence"
fi
echo "all three mutants built, ran and failed on numerical deformation assertions"
echo "UFBX_DEFORM_MUTANTS_OK"
