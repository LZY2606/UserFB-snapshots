#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-baseline}" in baseline|target) ;; *) exit 2 ;; esac
if [ "${1:-baseline}" = target ]; then test -s task_tests/CondyContract.java; fi
task_build=$(mktemp -d "${TMPDIR:-/tmp}/y1074-cfr.XXXXXX")
trap 'rm -rf "$task_build"' EXIT
bash task_support/build.sh "$task_build"
javac -encoding UTF-8 --release 8 -proc:none -classpath "$task_build/classes" -d "$task_build/classes" task_support/SyntheticCondy.java
java -cp "$task_build/classes" SyntheticCondy "$task_build/inputs"
java -cp "$task_build/classes" org.benf.cfr.reader.Main "$task_build/inputs/CondySample.class" --silent true > "$task_build/decompiled.txt"
cat "$task_build/decompiled.txt"
grep -q 'return 7;' "$task_build/decompiled.txt"
if [ "${1:-baseline}" = baseline ]; then
 echo CFR_BASELINE_OK
else
 find task_tests -name '*.java' -type f | LC_ALL=C sort > "$task_build/tests.txt"
 javac -encoding UTF-8 --release 8 -proc:none -classpath "$task_build/classes" -d "$task_build/classes" task_support/Contract.java @"$task_build/tests.txt"
 java -cp "$task_build/classes" Contract "$task_build/inputs"
fi
