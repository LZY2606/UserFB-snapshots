#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
task_build=${1:?build directory required}
mkdir -p "$task_build/classes" "$task_build/empty" "$task_build/generated/org/benf/cfr/reader/util"
sed -e 's/${project.version}/0.153-SNAPSHOT/g' -e 's/${git.commit.id.abbrev}/c414525/g' -e 's/${git.dirty}/false/g' src-templates/org/benf/cfr/reader/util/CfrVersionInfo.java > "$task_build/generated/org/benf/cfr/reader/util/CfrVersionInfo.java"
find src "$task_build/generated" -name '*.java' -type f | LC_ALL=C sort > "$task_build/sources.txt"
javac -encoding UTF-8 --release 8 -proc:none -classpath "$task_build/empty" -d "$task_build/classes" @"$task_build/sources.txt"
