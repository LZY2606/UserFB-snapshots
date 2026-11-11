#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash checks/legacy.sh
build-filter/filter_baseline --witness
