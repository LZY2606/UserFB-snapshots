#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/fauxgl-check.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
export GOPROXY=off GOTOOLCHAIN=local GOSUMDB=off GOPATH="$work/gopath" GOMODCACHE="$work/mod" GOCACHE="${TASK_GOCACHE:-$work/cache}"
if [ "${1:-baseline}" = baseline ]; then
 go test -count=1 -v . -run '^TestSeedBaseline$'
else
 test -f stroke_target_test.go || { echo 'missing stroke_target_test.go' >&2; exit 2; }
 go test -count=1 -v . -run '^(TestStrokePhase|TestStrokeJoins|TestStrokeDepth)$' > "$work/target.log" 2>&1 || { c=$?; cat "$work/target.log"; exit "$c"; }
 cat "$work/target.log"
 for name in TestStrokePhase TestStrokeJoins TestStrokeDepth; do
  grep -Eq "^--- PASS: $name \(" "$work/target.log" || { echo "not completed: $name" >&2; exit 3; }
  grep -Eq "^    --- PASS: $name/[^ ]+ \(" "$work/target.log" || { echo "no executed subtest: $name" >&2; exit 3; }
 done
 if grep -q -- '--- SKIP:' "$work/target.log"; then exit 4; fi
 echo STROKE_TARGET_COMPLETE
 go test -count=1 -v . -run '^TestSeedBaseline$'
fi
