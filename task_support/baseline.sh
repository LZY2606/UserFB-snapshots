#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/build.sh
build/task/baseline
build/task/old_tests
