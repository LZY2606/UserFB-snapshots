#!/bin/bash
set -euo pipefail
source support/build.sh
compile "$R/support/baseline.c" "$B/baseline"
"$B/baseline"
