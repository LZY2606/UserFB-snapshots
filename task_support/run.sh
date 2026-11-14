#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-target}
case "$mode" in baseline|target) ;; *) exit 2;; esac
work=$(mktemp -d "${TMPDIR:-/tmp}/check-javac.XXXXXX")
trap 'rm -rf "$work"' EXIT
find src/main/java vendor/jspecify/src/main/java task_support -name '*.java' -type f | LC_ALL=C sort > "$work/sources"
if [ "$mode" = target ]; then
  test -s contract/TextSourceRegression.java
  find contract -name '*.java' -type f | LC_ALL=C sort >> "$work/sources"
fi
javac -J-Djava.net.useSystemProxies=false -encoding UTF-8 -d "$work/classes" @"$work/sources"
if [ -d src/main/resources ]; then cp -R src/main/resources/. "$work/classes/"; fi
if [ "$mode" = baseline ]; then
  java -ea -cp "$work/classes" task_support.Baseline
else
  java -ea -cp "$work/classes" task_support.Driver "$work/receipt"
  test -s "$work/receipt"
  test "$(cat "$work/receipt")" = TEXT_SOURCE_ALL_OK
fi
