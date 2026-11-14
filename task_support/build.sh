#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
rm -rf .task-build/classes
mkdir -p .task-build/classes
find src/main/java task_support -name '*.java' -print | LC_ALL=C sort > .task-build/sources.txt
if [[ $mode == target ]]; then
  test -s contract/FormOwnerTest.java
  find contract -name '*.java' -print | LC_ALL=C sort >> .task-build/sources.txt
fi
javac --release 11 -encoding UTF-8 -d .task-build/classes @.task-build/sources.txt
