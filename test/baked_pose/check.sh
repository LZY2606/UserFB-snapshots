#!/bin/sh
# Player-style baked-animation regression.
#
# 1. Builds test/baked_pose/baked_pose.c against the pristine ufbx sources in
#    a temporary directory and runs it: the baked local TRS is composed into
#    world matrices along the node hierarchy and compared against independent
#    ufbx_evaluate_scene() results.
# 2. Builds three isolated copies of ufbx.c, each with one degradation, and
#    verifies that the same regression catches every one of them with a
#    numeric assertion failure (a compile failure or a source grep would not
#    count as detection).
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"

build=$(mktemp -d "${TMPDIR:-/tmp}/baked-pose.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

cc=${CC:-cc}
cflags="-std=c99 -O1"

echo "== pristine sources =="
$cc $cflags -I. test/baked_pose/baked_pose.c ufbx.c -lm -o "$build/baked_pose"
"$build/baked_pose"

# run_mutation <name> <perl-script> <pattern-that-must-be-gone>
run_mutation() {
	name=$1
	script=$2
	gone=$3
	dir="$build/mut-$name"
	mkdir -p "$dir"
	cp ufbx.c ufbx.h test/baked_pose/baked_pose.c "$dir/"
	printf '%s\n' "$script" > "$dir/mutate.pl"
	perl -0pi "$dir/mutate.pl" "$dir/ufbx.c"
	if grep -q "$gone" "$dir/ufbx.c"; then
		echo "ERROR: degradation '$name' did not apply to ufbx.c" >&2
		exit 1
	fi
	# A degradation that breaks the build is a bug in this script, not a detection.
	$cc $cflags -I"$dir" "$dir/baked_pose.c" "$dir/ufbx.c" -lm -o "$dir/baked_pose"
	set +e
	( cd "$root" && "$dir/baked_pose" ) > "$dir/log.txt" 2>&1
	status=$?
	set -e
	if [ "$status" -eq 0 ]; then
		echo "ERROR: degradation '$name' was NOT detected by the regression" >&2
		exit 1
	fi
	if ! grep -q '^FAIL ' "$dir/log.txt"; then
		echo "ERROR: degradation '$name' did not fail with a numeric assertion:" >&2
		cat "$dir/log.txt" >&2
		exit 1
	fi
	echo "MUTATION $name: detected -> $(grep '^FAIL ' "$dir/log.txt" | head -n 1)"
}

# 1. Step sampling at identical times no longer recognizes the left/right
#    limit flags on baked keys.
run_mutation step-flags '
s/\t\tif \(prev > keys && \(prev->flags & UFBX_BAKED_KEY_STEP_RIGHT\) != 0 && prev\[-1\]\.time == time\) prev--;\n//g;
s/\t\tif \(prev->flags & UFBX_BAKED_KEY_STEP_LEFT\) t = 0\.0;\n//g;
s/\t\tif \(next->flags & UFBX_BAKED_KEY_STEP_RIGHT\) t = 1\.0;\n//g;
' 'UFBX_BAKED_KEY_STEP_LEFT) t = 0\.0'

# 2. Baking drops the animated parent scale-helper compensation from the
#    sibling translation/scale keys.
run_mutation scale-helper '
s/transform\.translation\.x \*= scale\.x;/transform.translation.x *= 1.0;/;
s/transform\.translation\.y \*= scale\.y;/transform.translation.y *= 1.0;/;
s/transform\.translation\.z \*= scale\.z;/transform.translation.z *= 1.0;/;
s/transform\.scale\.x \*= scale\.x;/transform.scale.x *= 1.0;/;
s/transform\.scale\.y \*= scale\.y;/transform.scale.y *= 1.0;/;
s/transform\.scale\.z \*= scale\.z;/transform.scale.z *= 1.0;/;
' 'transform\.translation\.x \*= scale\.x'

# 3. Quaternion interpolation no longer treats q and -q as the same pose.
run_mutation quat-sign '
s/\tif \(dot < 0\.0\) \{\n\t\tdot = -dot;\n\t\tb\.x = -b\.x; b\.y = -b\.y; b\.z = -b\.z; b\.w = -b\.w;\n\t\}\n//;
' 'dot = -dot'

echo "PASS check.sh: pristine sources pass, all 3 degradations detected"
