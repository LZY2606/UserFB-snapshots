#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=.task-build
case "$mode" in
baseline)
    cmake -S task_support -B "$build" -DTASK_TARGET=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build" --target starting_point old_tests --parallel 2
    "$build/starting_point"
    "$build/old_tests" --reporter compact > "$build/old.log" 2>&1
    cat "$build/old.log"
    grep -Eq '^TEST_RUN_COMPLETE cases=[1-9][0-9]* assertions=[1-9][0-9]*$' "$build/old.log"
    echo DETOUR_OLD_TESTS_OK
    ;;
target)
    build=$(mktemp -d .task-target.XXXXXX)
    trap 'rm -rf "$build"' EXIT
    cmake -S task_support -B "$build" -DTASK_TARGET=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build" --target weighted_tests old_tests starting_point --parallel 2
    for group in optimality portals certificate integration; do
        log="$build/weighted-$group.log"
        "$build/weighted_tests" "[weighted][$group]" --reporter compact > "$log" 2>&1 || { cat "$log"; exit 1; }
        cat "$log"
        grep -Eq '^TEST_RUN_COMPLETE cases=[1-9][0-9]* assertions=[1-9][0-9]*$' "$log"
        echo "WEIGHTED_GROUP_OK $group"
    done
    "$build/old_tests" --reporter compact > "$build/old.log" 2>&1
    cat "$build/old.log"
    grep -Eq '^TEST_RUN_COMPLETE cases=[1-9][0-9]* assertions=[1-9][0-9]*$' "$build/old.log"
    "$build/starting_point"
    echo WEIGHTED_CORRIDOR_COMPLETE
    ;;
*) echo "unknown mode: $mode" >&2; exit 2;;
esac
