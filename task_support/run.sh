#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
b=$(mktemp -d "${TMPDIR:-/tmp}/multicue-build.XXXXXX")
trap 'rm -rf "$b"' EXIT HUP INT TERM
mode=${1:-baseline}
if [ "$mode" = baseline ]; then
 set -- "$root/task_support/baseline.cc"
elif [ "$mode" = target ]; then
 test -s "$root/testing/multitrack_cues_test.cc"
 set -- "$root/task_support/driver.c" "$root/testing/multitrack_cues_test.cc"
else exit 2; fi
c++ -std=c++11 -O1 -I "$root" "$@" "$root"/mkvmuxer/*.cc "$root"/mkvparser/*.cc -o "$b/probe"
cd "$b"
set +e
./probe > "$b/output.log" 2>&1
status=$?
set -e
cat "$b/output.log"
[ "$status" -eq 0 ] || exit "$status"
if [ "$mode" = target ]; then
 grep -Eq '^TARGET checks=[1-9][0-9]*$' "$b/output.log"
fi
