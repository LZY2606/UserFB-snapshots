#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=baseline
if [[ $# -gt 0 ]]; then mode=$1; fi
bash task_support/env.sh bash task_support/checks.sh "$mode"
