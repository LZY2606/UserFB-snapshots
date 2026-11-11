#!/usr/bin/env bash
set -euo pipefail

# Verifies that telemetry_ingress_test.go actually guards the production
# telemetry ingress rules. Each degradation patch is applied to its own
# throwaway copy of the source tree; the real working tree is never touched.
#
# A scenario only counts as CAUGHT when:
#   * the target package (and its test binary) compiles after the patch,
#   * TestTelemetryIngressContract runs at least one case,
#   * the expected subtest fails, and
#   * the failure log contains the behavior assertion specific to the patch.
# Compile failures, timeouts, missing files and zero test cases are rejected.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
PATCH_DIR="$SCRIPT_DIR/patches"
LOG_DIR="$SCRIPT_DIR/logs"

TEST_PATTERN='^TestTelemetryIngressContract$'
TEST_TIMEOUT='120s'

# Use the same temporary caches as the acceptance command. Dedicated
# TELEMETRY_* variables can override them, but inherited GOMODCACHE/GOCACHE
# must not leak in (the workspace may point them at an empty directory).
export GOMODCACHE="${TELEMETRY_GOMODCACHE:-${TMPDIR:-/tmp}/mod}"
export GOCACHE="${TELEMETRY_GOCACHE:-${TMPDIR:-/tmp}/build}"
export GOWORK=off

mkdir -p "$LOG_DIR"

tmpdirs=()
cleanup() {
	if ((${#tmpdirs[@]} > 0)); then
		rm -rf "${tmpdirs[@]}"
	fi
}
trap cleanup EXIT

copy_tree() {
	local dst=$1
	mkdir -p "$dst"

	if command -v rsync >/dev/null 2>&1; then
		rsync -a --exclude=.git --exclude=checks/logs "$REPO_ROOT/" "$dst/"
	else
		(
			cd "$REPO_ROOT"
			tar --exclude=.git --exclude=checks/logs -cf - .
		) | (
			cd "$dst"
			tar -xf -
		)
	fi
}

# grep helpers that do not abort the script when there is no match.
has_text() {
	grep -Fq -- "$2" "$1"
}

has_regex() {
	grep -Eq -- "$2" "$1"
}

run_scenario() {
	local patch_name=$1
	local scenario=$2
	local expected_token=$3
	local expected_subtest=$4

	local patch_path="$PATCH_DIR/$patch_name"
	local patch_stem=${patch_name%.patch}
	local run_log="$LOG_DIR/$patch_stem.log"
	local compile_log="$LOG_DIR/$patch_stem.compile.log"
	local tmp
	local status

	echo
	echo "SCENARIO: $scenario"
	echo "PATCH: $patch_name"
	echo "TEST: TestTelemetryIngressContract/$expected_subtest"

	if [[ ! -f "$patch_path" ]]; then
		echo "RESULT: NOT CAUGHT (missing patch file: $patch_path)"
		return 1
	fi

	tmp=$(mktemp -d "${TMPDIR:-/tmp}/verify-telemetry.$patch_stem.XXXXXX")
	tmpdirs+=("$tmp")
	copy_tree "$tmp"

	if ! (cd "$tmp" && git init -q && git apply -p1 "$patch_path") >"$compile_log" 2>&1; then
		echo "RESULT: NOT CAUGHT (patch did not apply; see $compile_log)"
		return 1
	fi

	# Compile the target package and its test binary. Running zero tests is
	# enough here; a failure at this stage must never count as a catch.
	if ! (cd "$tmp" && go test -run '^$' -count=1 .) >>"$compile_log" 2>&1; then
		echo "RESULT: NOT CAUGHT (target package did not compile; see $compile_log)"
		return 1
	fi

	# Run the contract test. The exit status alone is not trusted: we also
	# require a real failed subtest and the expected assertion text.
	if (cd "$tmp" && go test -run "$TEST_PATTERN" -v -count=1 -timeout "$TEST_TIMEOUT" .) >"$run_log" 2>&1; then
		status=0
	else
		status=$?
	fi

	if ((status == 124)); then
		echo "RESULT: NOT CAUGHT (test run hit the external timeout)"
		return 1
	fi

	if has_regex "$run_log" 'panic: test timed out|test timed out after|^FAIL.*\[build failed\]' ||
		has_text "$run_log" '[build failed]' ||
		has_text "$run_log" '[setup failed]' ||
		has_text "$run_log" 'no tests to run' ||
		has_text "$run_log" 'no test files' ||
		has_regex "$run_log" '^# |cannot find package|undefined:' ; then
		echo "RESULT: NOT CAUGHT (compile/setup/timeout/zero-test failure; see $run_log)"
		return 1
	fi

	if ! has_text "$run_log" '=== RUN   TestTelemetryIngressContract'; then
		echo "RESULT: NOT CAUGHT (TestTelemetryIngressContract did not run; see $run_log)"
		return 1
	fi

	if ! has_text "$run_log" "--- FAIL: TestTelemetryIngressContract/$expected_subtest "; then
		echo "RESULT: NOT CAUGHT (expected subtest $expected_subtest did not fail; see $run_log)"
		return 1
	fi

	if ! has_text "$run_log" "$expected_token"; then
		echo "RESULT: NOT CAUGHT (failure was not the expected behavior assertion; see $run_log)"
		return 1
	fi

	if ((status == 0)); then
		echo "RESULT: NOT CAUGHT (test command exited successfully; see $run_log)"
		return 1
	fi

	echo "RESULT: CAUGHT (behavior assertion failed: \"$expected_token\")"
	echo "LOG: $run_log"
	return 0
}

failures=0

run_scenario \
	"01-datagram-buffer-retained.patch" \
	"datagram buffer retained: stale bytes must not become the next packet's frame" \
	"datagram leftover bytes leaked into next packet" \
	"datagram_one_frame_per_packet" || failures=$((failures + 1))

run_scenario \
	"02-extension-padding-nonzero.patch" \
	"missing extension bytes filled with non-zero padding" \
	"omitted extension field decoded as non-zero" \
	"stream_fragmented_and_coalesced" || failures=$((failures + 1))

run_scenario \
	"03-crc-check-skipped.patch" \
	"known-message CRC validation skipped" \
	"known message with bad CRC was published" \
	"stream_fragmented_and_coalesced" || failures=$((failures + 1))

echo
if ((failures > 0)); then
	echo "SUMMARY: $failures scenario(s) were not caught"
	exit 1
fi

echo "SUMMARY: all degradation scenarios were caught by behavior assertions"
