#!/usr/bin/env bash
# Build and run the original QuickHull tests and the incremental hull harness,
# both in a temporary directory.  Returns non-zero if a file is missing, a
# compilation/test step fails, the original test suite prints a "test failed"
# diagnostic, or any required PASS marker is absent.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/quickhull-incremental.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

required_files=(
	"QuickHull.hpp"
	"QuickHull.cpp"
	"HullExpansion.hpp"
	"IncrementalHull.hpp"
	"IncrementalHull.cpp"
	"Tests/main.cpp"
	"Tests/QuickHullTests.cpp"
	"harness/incremental.cpp"
)

for file in "${required_files[@]}"; do
	if [[ ! -f "$root/$file" ]]; then
		echo "missing required file: $file" >&2
		exit 1
	fi
done

# ---------------------------------------------------------------------------
# 1. Original one-shot test suite.  Note that its VERIFY macro only prints
#    "test failed" and does not change the exit code, so scan the log as well.
# ---------------------------------------------------------------------------
c++ -std=c++14 -O1 -I "$root" \
	"$root/QuickHull.cpp" \
	"$root/Tests/main.cpp" \
	"$root/Tests/QuickHullTests.cpp" \
	-o "$tmp/upstream"

if ! "$tmp/upstream" > "$tmp/upstream.log" 2>&1; then
	cat "$tmp/upstream.log"
	echo "original QuickHull test binary failed" >&2
	exit 1
fi
cat "$tmp/upstream.log"

if grep -q 'test failed' "$tmp/upstream.log"; then
	echo "original QuickHull tests reported a failure" >&2
	exit 1
fi
if ! grep -q 'QuickHull tests succesfully passed.' "$tmp/upstream.log"; then
	echo "original QuickHull tests did not report success" >&2
	exit 1
fi

# ---------------------------------------------------------------------------
# 2. Incremental hull harness.
# ---------------------------------------------------------------------------
c++ -std=c++14 -O1 -I "$root" \
	"$root/QuickHull.cpp" \
	"$root/IncrementalHull.cpp" \
	"$root/harness/incremental.cpp" \
	-o "$tmp/incremental"

if ! "$tmp/incremental" > "$tmp/incremental.log" 2>&1; then
	cat "$tmp/incremental.log"
	echo "incremental hull harness failed" >&2
	exit 1
fi
cat "$tmp/incremental.log"

if grep -q 'test failed' "$tmp/incremental.log"; then
	echo "incremental hull harness reported a failure" >&2
	exit 1
fi

for marker in 'PASS hull-geometry' 'PASS draw-delta' 'PASS storage-lifetime'; do
	if ! grep -q "^${marker}$" "$tmp/incremental.log"; then
		echo "missing expected output: $marker" >&2
		exit 1
	fi
done

if ! grep -q '^QUICKHULL_INCREMENTAL_OK$' "$tmp/incremental.log"; then
	echo "incremental hull harness did not report overall success" >&2
	exit 1
fi

echo QUICKHULL_INCREMENTAL_OK
