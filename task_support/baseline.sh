#!/usr/bin/env bash
set -euo pipefail
TASK_BUILD=$(mktemp -d); export TASK_BUILD
trap 'rm -rf "$TASK_BUILD"' EXIT
bash task_support/build.sh
c++ -std=c++17 -O1 -I. -Itask_support task_support/baseline.cpp "$TASK_BUILD/libschrift.a" -lm -o "$TASK_BUILD/baseline"
"$TASK_BUILD/baseline" "$TASK_BUILD/rectangle.ttf"
