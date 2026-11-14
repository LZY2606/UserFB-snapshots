#!/bin/bash
# Regression guard for crawl_protocol_target_test.go.
#
# For each of three single, compilable degradations it:
#   1. copies the whole repository into a private, throwaway directory,
#   2. applies exactly that one mutation to the copy,
#   3. runs ONLY the new protocol tests that need no network socket
#      (Admission and Runs),
#   4. requires a failure produced by a content assertion inside
#      crawl_protocol_target_test.go - never a build failure, panic or
#      timeout, and never by grepping production source.
#
# The original working tree is never written to; its bytes are checksummed
# before and after and must match. All failure logs are kept in a directory
# that is printed at the end.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOENV=off GOFLAGS=-mod=vendor

# Keep all throwaway state under .task-build, which is excluded from both the
# tree checksum and the repository copies.
base_work="$repo_root/.task-build/mutation-check"
rm -rf "$base_work"
work="$base_work/run"
log_dir="$work/logs"
mkdir -p "$log_dir"
# Also expose the logs under a stable, printed path.
stable_log_dir="$base_work/logs"
rm -rf "$stable_log_dir"
mkdir -p "$stable_log_dir"
# Throwaway copies and caches are removed on exit; the printed log directory
# (base_work/logs) is deliberately kept.
trap 'rm -rf "$work"' EXIT

new_test_re='^(TestCrawlProtocolAdmission|TestCrawlProtocolRuns)$'
mutations=(robots-bypass requestget-veto-ignored visited-harvest-nil)

# Snapshot the working tree (tracked and untracked files, excluding caches).
snapshot_tree() {
	find . -path ./.task-build -prune -o -path ./.git -prune -o -type f -print0 \
		| sort -z | xargs -0 shasum -a 256 | shasum -a 256
}
before_hash="$(snapshot_tree)"

# Build the mutator once, outside every copy, into the private work dir.
GOCACHE="$work/go-cache" go build -o "$work/mutator" ./task_support/mutator

make_copy() {
	local dst="$1"
	mkdir -p "$dst"
	tar --exclude='./.task-build' --exclude='./.git' -cf - . | tar -xf - -C "$dst"
}

# Control: an unmutated copy must pass the same two test groups.
control_dir="$work/control"
make_copy "$control_dir"
control_log="$log_dir/00-control.log"
set +e
( cd "$control_dir" && GOCACHE="$work/go-cache" go test -vet=off -race -count=1 -timeout=60s -run "$new_test_re" . ) >"$control_log" 2>&1
control_status=$?
set -e
if [[ $control_status -ne 0 ]]; then
	echo "control copy failed before any mutation (see $control_log)"
	cat "$control_log"
	exit 1
fi
echo "control: unmutated copy passes"

failures=0
for name in "${mutations[@]}"; do
	copy_dir="$work/copy-$name"
	make_copy "$copy_dir"

	# Exactly one mutation, applied to the copy only.
	"$work/mutator" "$copy_dir" "$name"

	log="$log_dir/$name.log"
	set +e
	( cd "$copy_dir" && GOCACHE="$work/go-cache" go test -vet=off -race -count=1 -timeout=60s -run "$new_test_re" . ) >"$log" 2>&1
	status=$?
	set -e

	problem=""
	if [[ $status -eq 0 ]]; then
		problem="the degraded build still passed (the mutation was not detected)"
	elif grep -q "panic:" "$log"; then
		problem="failure was a panic, not a content assertion"
	elif grep -q "test timed out" "$log"; then
		problem="failure was a timeout, not a content assertion"
	elif grep -Eq "\[build failed\]|^# .* \[.*\]|does not compile|undefined:" "$log"; then
		problem="failure was a build error, not a content assertion"
	elif ! grep -Eq "crawl_protocol_target_test\.go:[0-9]+:" "$log"; then
		problem="no content-assertion line from crawl_protocol_target_test.go was found"
	fi

	if [[ -n "$problem" ]]; then
		echo "MUTATION $name: NOT CAUGHT PROPERLY -> $problem"
		failures=$((failures + 1))
	else
		# Show the concrete assertion messages that detected the degradation.
		echo "MUTATION $name: caught by content assertion(s):"
		grep -E "crawl_protocol_target_test\.go:[0-9]+:" "$log" | sed 's/^/    /'
	fi
done

# Mirror the logs to the stable, printed location and dump the tree hash check.
cp -R "$log_dir/." "$stable_log_dir/"
after_hash="$(snapshot_tree)"
echo
echo "failure logs: $stable_log_dir"
echo "working-tree checksum before: $before_hash"
echo "working-tree checksum after:  $after_hash"

if [[ "$before_hash" != "$after_hash" ]]; then
	echo "WORKING TREE CHANGED while running the mutation check"
	exit 1
fi
if [[ $failures -ne 0 ]]; then
	echo "$failures mutation(s) were not caught by content assertions"
	exit 1
fi
echo "all ${#mutations[@]} degradations are caught by the new content assertions; working tree bytes unchanged"
