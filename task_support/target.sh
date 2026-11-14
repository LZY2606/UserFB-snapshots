#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/run.sh target
source_root=$PWD
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
for name in slope stair; do
    mutant="$scratch/$name"
    mkdir -p "$mutant"
    tar --exclude=.git --exclude=.task-build -cf - . | tar -xf - -C "$mutant"
    (
        cd "$mutant"
        git apply "$source_root/task_support/mutations/$name.patch"
        mkdir -p .task-build
        cc -std=c99 -O2 -I. -Itask_support task_support/driver.c tests/floor1_contract.c -lm -o .task-build/target
    ) >"$scratch/$name-build.log" 2>&1 || {
        tail -12 "$scratch/$name-build.log" >&2
        echo "MUTANT_BUILD_FAILED $name" >&2
        exit 70
    }
    status=0
    (cd "$mutant" && .task-build/target) >"$scratch/$name-test.log" 2>&1 || status=$?
    if [[ "$status" != 4 ]] || ! grep -q '^FAIL ' "$scratch/$name-test.log"; then
        tail -12 "$scratch/$name-test.log" >&2
        echo "MUTANT_NOT_CONTENT_FAILURE $name exit=$status" >&2
        exit 71
    fi
    echo "MUTANT_DETECTED $name exit=$status"
    cat "$scratch/$name-test.log"
done
