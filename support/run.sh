#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
TASK_BUILD=$(mktemp -d "${TMPDIR:-/tmp}/java.XXXXXX")
trap 'rm -rf "$TASK_BUILD"' EXIT HUP INT TERM
mode=${1:-}
case "$mode" in
baseline) ;;
target) test -s support/ShadowRootRegression.java || { echo 'required regression missing/empty' >&2; exit 2; } ;;
*) echo 'usage: sh support/run.sh baseline|target' >&2; exit 2;;
esac
find src/main/java support/vendor -name '*.java' -type f ! -path '*/super/*' > "$TASK_BUILD/sources"
printf '%s\n' support/CaseHarness.java support/LegacySmoke.java support/Baseline.java >> "$TASK_BUILD/sources"
if [ "$mode" = target ]; then printf '%s\n' support/ShadowRootRegression.java >> "$TASK_BUILD/sources"; fi
javac -encoding UTF-8 --release 11 -d "$TASK_BUILD/classes" @"$TASK_BUILD/sources"
if [ "$mode" = baseline ]; then java -cp "$TASK_BUILD/classes" Baseline; else
 java -cp "$TASK_BUILD/classes" LegacySmoke
 java -cp "$TASK_BUILD/classes" CaseHarness ShadowRootRegression testShadow
fi
