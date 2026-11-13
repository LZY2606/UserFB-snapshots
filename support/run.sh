#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/uf-java.XXXXXX")
trap 'rm -rf "$b"' EXIT
find jOOL/src/main/java -name '*.java' -print > "$b/sources"
printf '%s\n' support/Baseline.java support/UFHarness.java >> "$b/sources"
case "${1:-baseline}" in
 baseline)
 javac --release 11 -proc:none -cp "$b/classes" -d "$b/classes" @"$b/sources"
 java -cp "$b/classes" org.jooq.lambda.support.Baseline
 ;;
 target)
 printf '%s\n' jOOL/src/test/java/org/jooq/lambda/SpillSortContract.java >> "$b/sources"
 javac --release 11 -proc:none -cp "$b/classes" -d "$b/classes" @"$b/sources"
 java -cp "$b/classes" org.jooq.lambda.support.UFHarness > "$b/output"
 cat "$b/output"
 for g in normal boundary integration; do grep -Eq "^GROUP $g PASS cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$b/output"; done
 grep -Eq '^UNIT_TARGET_COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$b/output"
 java -cp "$b/classes" org.jooq.lambda.support.Baseline
 ;;
 *) exit 2;;
esac
