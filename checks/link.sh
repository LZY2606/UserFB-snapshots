#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
output=$1
shift
case "$(uname -s)" in Darwin) strip_flag=-Wl,-dead_strip;; *) strip_flag=-Wl,--gc-sections;; esac
"${CC:-cc}" -std=c99 -O1 -ffunction-sections -fdata-sections -I. "$@" .task-build/renderer.a "$strip_flag" -lm -o "$output"
