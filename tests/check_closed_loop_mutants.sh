#!/bin/bash
# tests/check_closed_loop_mutants.sh
#
# Mutation check for tests/closed_loop.c.
#
# Two mutants are built in throw-away copies of the source tree; the
# originals are never modified:
#
#   1. LTP lag search: the strict correlation maximum comparison
#      "L_result > L_max" in the long term predictor becomes ">=".
#      This keeps the first *equal* maximum from being the winner only
#      when a later lag is strictly larger; with ">=" the last equal
#      lag wins and the repeated-period fixtures pick a different Nc.
#
#   2. Closed loop feedback in code.c: the reconstructed short term
#      residual that is fed back into dp[] is replaced by the current
#      original short term residual.  This is the classic analysis-loop
#      bug: the predictor history must carry quantised reconstruction,
#      not the unquantised residual.
#
# Each mutant must (a) compile, (b) run to completion, and (c) be
# rejected by the suite: the test binary must print CONTENT_MISMATCH and
# the runner must exit with status 4.  A build failure or an abnormal
# termination (signal, timeout, any other status) is NOT a detection.
#
# The script only uses the system compiler, ar and coreutils/BSD tools.
# It writes into $TMPDIR (falling back to the build directory) and never
# touches src/, inc/, tests/ or task_support/ of the real tree.
#
# Exit status: 0 when both mutants are detected, 1 otherwise.

set -u

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)

work=$(mktemp -d "${TMPDIR:-/tmp}/cl-mutants.XXXXXX") || {
	echo "FAIL cannot create temporary directory" >&2
	exit 1
}
trap 'rm -rf "$work"' EXIT HUP INT TERM

flags=(-std=c99 -O2 -Wno-deprecated-non-prototype -Wno-comment
       -DSASR -DWAV49 -DNeedFunctionPrototypes=1)
sources=(add code debug decode long_term lpc preprocess rpe gsm_destroy
	 gsm_decode gsm_encode gsm_explode gsm_implode gsm_create gsm_print
	 gsm_option short_term table)

# Build one mutated tree in $work/$name and run the closed loop suite.
# Returns 0 when the mutant is detected (CONTENT_MISMATCH + exit 4),
# 1 when it is not, and 2 for an infrastructure problem (build failure
# or abnormal termination), which is reported but not counted as a
# detection.
run_mutant() {
	local name=$1
	local srcfile=$2
	local old=$3
	local new=$4
	local dir="$work/$name"

	mkdir -p "$dir" || return 2
	cp -R "$repo/src" "$repo/inc" "$repo/task_support" "$repo/tests" \
	      "$dir/" || return 2

	# Apply the mutation with a tiny C program so no sed/awk is
	# needed and the match count can be verified exactly.
	cc -std=c99 -I"$repo/task_support" \
	   -o "$work/patch_$name" "$repo/tests/patch_source.c" 2>/dev/null ||
	   return 2
	"$work/patch_$name" "$dir/$srcfile" "$old" "$new" || return 2

	mkdir -p "$dir/obj" || return 2
	local f
	for f in "${sources[@]}"; do
		cc "${flags[@]}" -I"$dir" -I"$dir/inc" \
		   -c "$dir/src/$f.c" -o "$dir/obj/$f.o" || return 2
	done
	ar rcs "$dir/lib.a" "$dir"/obj/*.o || return 2
	cc "${flags[@]}" -I"$dir" -I"$dir/inc" \
	   "$dir/task_support/driver.c" "$dir/tests/closed_loop.c" \
	   "$dir/lib.a" -lm -o "$dir/target" || return 2

	"$dir/target" >"$dir/out" 2>&1
	target_status=$?

	if [ "$target_status" -eq 4 ] && grep -q '^CONTENT_MISMATCH$' "$dir/out"; then
		return 0		# rejected with the required marker
	fi

	# Not rejected.  A build failure, a signal or any status other than
	# the runner's own 0/1 codes means the mutant did not run normally,
	# so it must not be recorded as a detection.
	case $target_status in
	0|1)	return 1 ;;	# ran and was accepted / mis-handled
	*)	echo "mutant $name terminated abnormally (status $target_status)" >&2
		return 2 ;;
	esac
}

status=0

check_one() {
	local name=$1 srcfile=$2 old=$3 new=$4
	local rc
	if run_mutant "$name" "$srcfile" "$old" "$new"; then
		echo "DETECTED $name"
		return 0
	fi
	rc=$?
	echo "MISSED $name (rc=$rc)" >&2
	status=1
	return 0
}

check_one ltp_ge src/long_term.c \
	'if (L_result > L_max) {' \
	'if (L_result >= L_max) {'

check_one feedback src/code.c \
	'dp[ i ] = GSM_ADD( S->e[5 + i], dpp[i] );' \
	'dp[ i ] = so[ k * 40 + i ];'

exit $status
