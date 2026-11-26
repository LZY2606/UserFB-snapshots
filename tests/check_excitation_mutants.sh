#!/usr/bin/env bash
# Verify that the excitation reference tests catch the two required semantic
# mutations.  All edits happen in disposable copies; this script never edits
# the source tree it was launched from.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/excitation-mutants.XXXXXX")
trap 'rm -rf "$work"' EXIT

# Some restricted macOS sandboxes provide a TMPDIR that /usr/bin/mktemp's
# argument-free form does not honor.  Give the copied run.sh a small wrapper
# while leaving normal systems unaffected.
mkdir -p "$work/bin"
cat >"$work/bin/mktemp" <<'EOF'
#!/usr/bin/env bash
if [ "$#" -eq 0 ]; then
	exec /usr/bin/mktemp "${TMPDIR:-/tmp}/task-run.XXXXXX"
fi
exec /usr/bin/mktemp "$@"
EOF
chmod +x "$work/bin/mktemp"

run_variant() {
	local name=$1
	local relative_source=$2
	local sed_program=$3
	local variant="$work/$name"
	local output status

	mkdir -p "$variant"
	cp -R "$root/task_support" "$root/src" "$root/include" "$root/tests" "$variant/"

	sed "$sed_program" "$root/$relative_source" >"$variant/$relative_source"
	if cmp -s "$root/$relative_source" "$variant/$relative_source"; then
		echo "MUTANT_SETUP_FAILED $name: source was not changed" >&2
		return 2
	fi

	set +e
	output=$(
		cd "$variant" &&
		PATH="$work/bin:$PATH" bash task_support/run.sh target 2>&1
	)
	status=$?
	set -e

	printf '%s\n' "$output" | grep -E 'CONTENT_MISMATCH|CASE |GROUP |FAIL ' | tail -20 || true
	if [ "$status" -ne 4 ]; then
		echo "MUTANT_NOT_DETECTED $name: expected runner exit 4, got $status" >&2
		if [ -n "${output:-}" ]; then
			printf '%s\n' "$output" | tail -80 >&2
		fi
		return 1
	fi
	if ! printf '%s\n' "$output" | grep -q 'CONTENT_MISMATCH'; then
		echo "MUTANT_NOT_DETECTED $name: exit 4 without CONTENT_MISMATCH" >&2
		return 1
	fi
	echo "MUTANT_DETECTED $name exit=4 CONTENT_MISMATCH"
}

# Reverse the +1 base-delay adjustment used specifically for fracPitchDelay=+1.
run_variant \
	"positive-fraction-base" \
	"src/decodeAdaptativeCodeVector.c" \
	's/-(intPitchDelay+1)/-(intPitchDelay-1)/'

# Invert the extracted jx track bit after the production bit extraction.
run_variant \
	"last-pulse-track-bit" \
	"src/decodeFixedCodeVector.c" \
	'/jx = positions&(uint16_t)1;/s/$/ jx ^= 1;/'

echo "EXCITATION_MUTANTS_OK"
