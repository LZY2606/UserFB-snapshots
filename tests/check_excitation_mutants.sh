#!/bin/bash
#
# check_excitation_mutants.sh
#
# Mutation check for tests/excitation_reference.c. Two independent, compilable
# variants of the production sources are built in temporary copies:
#
#   mutant A: the positive fractional delay base point adjustment in
#             decodeAdaptativeCodeVector.c is inverted (the +1 integer lag
#             adjustment becomes -1);
#   mutant B: the last-pulse track selection bit jx in
#             decodeFixedCodeVector.c is inverted.
#
# The same excitation regression must detect each variant through a numeric
# content difference: the run prints CONTENT_MISMATCH and the runner exits
# with status 4. A compile failure or a crash is not a valid detection.
#
# The original source tree is never modified: every variant lives in a
# temporary directory and the two targeted source files are checksummed
# before and after the run.
#
# The script deliberately does not rely on mktemp: working directories are
# created from the process id under TMPDIR and removed by a cleanup trap, so
# environment specific mktemp shims cannot interfere with the -d form.
#
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
work_root=${TMPDIR:-/tmp}

adaptive_src=src/decodeAdaptativeCodeVector.c
fixed_src=src/decodeFixedCodeVector.c
targets=("$adaptive_src" "$fixed_src")

checksum_files() {
	for f in "$@"; do
		shasum -a 256 "$repo_root/$f"
	done
}

before=$(checksum_files "${targets[@]}")
copy_dirs=()
log_files=()
cleanup() {
	local d f
	for d in "${copy_dirs[@]:-}"; do
		[[ -n $d ]] && rm -rf "$d"
	done
	for f in "${log_files[@]:-}"; do
		[[ -n $f ]] && rm -f "$f"
	done
}
trap cleanup EXIT

# apply_mutant <copy directory> <mutant name>
apply_mutant() {
	local copy_dir=$1
	local mutant=$2
	if [[ $mutant == fractional_base_point ]]; then
		# -(intPitchDelay+1) -> -(intPitchDelay-1) : invert the sign
		# of the positive fractional base point adjustment.
		sed -i '' \
			's#excitationVector\[-(intPitchDelay+1)\]#excitationVector[-(intPitchDelay-1)]#' \
			"$copy_dir/$adaptive_src"
		grep -q 'excitationVector\[-(intPitchDelay-1)\]; /\* fracPitchDelay being positive' \
			"$copy_dir/$adaptive_src"
	elif [[ $mutant == fixed_codebook_track_bit ]]; then
		# jx bit flipped: track selection between offsets 3 and 4 is
		# inverted for every frame.
		sed -i '' \
			's#jx = positions&(uint16_t)1;#jx = (positions\&(uint16_t)1)^(uint16_t)1;#' \
			"$copy_dir/$fixed_src"
		grep -q 'jx = (positions&(uint16_t)1)\^(uint16_t)1;' \
			"$copy_dir/$fixed_src"
	else
		echo "unknown mutant: $mutant" >&2
		exit 2
	fi
}

# run_mutant <mutant name>; returns 0 only on a content-based detection.
run_mutant() {
	local mutant=$1
	local copy_dir log status
	copy_dir="$work_root/excitation_mutant.${mutant}.$$"
	log="$work_root/excitation_log.${mutant}.$$"
	rm -rf "$copy_dir"
	mkdir -p "$copy_dir"
	copy_dirs+=("$copy_dir")
	log_files+=("$log")

	# only the directories the build actually needs are copied.
	cp -R "$repo_root/src" "$repo_root/include" \
		"$repo_root/task_support" "$repo_root/tests" "$copy_dir"

	apply_mutant "$copy_dir" "$mutant"

	set +e
	( cd "$copy_dir" && bash task_support/run.sh target ) >"$log" 2>&1
	status=$?
	set -e
	cat "$log"

	if [[ $status -ge 128 ]]; then
		echo "MUTANT $mutant: CRASH (status $status), not a content detection" >&2
		return 1
	fi
	if [[ $status -ne 4 ]]; then
		echo "MUTANT $mutant: expected runner exit 4, got $status" >&2
		return 1
	fi
	if ! grep -q '^CONTENT_MISMATCH ' "$log"; then
		echo "MUTANT $mutant: exit 4 without CONTENT_MISMATCH output" >&2
		return 1
	fi
	echo "MUTANT $mutant: detected via CONTENT_MISMATCH (exit 4)"
	return 0
}

rc=0
run_mutant fractional_base_point || rc=1
run_mutant fixed_codebook_track_bit || rc=1

after=$(checksum_files "${targets[@]}")
if [[ $before != "$after" ]]; then
	echo "original production sources were modified" >&2
	rc=1
fi

if [[ $rc -eq 0 ]]; then
	echo "EXCITATION_MUTANTS_OK mutants=2"
fi
exit $rc
