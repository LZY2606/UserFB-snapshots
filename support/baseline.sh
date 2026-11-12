#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/build.sh"
"${CC:-cc}" "${cc_args[@]}" "${sources[@]}" support/baseline.c -lm -o "$build_dir/baseline"
"$build_dir/baseline"
bash support/legacy.sh
