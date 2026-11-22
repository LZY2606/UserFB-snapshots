#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -rf .task-build/classes
mkdir -p .task-build/classes
find core/src/main/java -name '*.java' ! -name 'module-info.java' -print | LC_ALL=C sort > .task-build/production.list
javac --release 11 -encoding UTF-8 -d .task-build/classes @.task-build/production.list task_support/Baseline.java task_support/LegacyPitchShifter.java
