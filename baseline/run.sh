#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -x baseline/.build/hrtf_witness
test -x baseline/.build/upstream_selected
baseline/.build/hrtf_witness
baseline/.build/upstream_selected
