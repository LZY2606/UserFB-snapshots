#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in
  baseline)
    bash task_support/build.sh
    exec java --add-modules jdk.httpserver -cp .task-build/classes Baseline
    ;;
  target)
    test -s src/test/java/crawlercommons/sitemaps/WalkGraphRegression.java || { echo 'missing WalkGraphRegression.java' >&2; exit 2; }
    rm -rf .task-build
    bash task_support/build.sh
    javac -encoding UTF-8 --release 11 --add-modules jdk.httpserver -classpath .task-build/classes -sourcepath src/main/java:src/test/java:task_support -d .task-build/classes src/test/java/crawlercommons/sitemaps/WalkGraphRegression.java
    java --add-modules jdk.httpserver -cp .task-build/classes Compatibility
    java --add-modules jdk.httpserver -cp .task-build/classes TargetDriver > .task-build/target.log 2>&1 || { result_code=$?; cat .task-build/target.log; exit "$result_code"; }
    cat .task-build/target.log
    grep -qx WALK_TARGET_COMPLETE .task-build/target.log
    ;;
  *) echo 'use baseline or target' >&2; exit 2 ;;
esac
