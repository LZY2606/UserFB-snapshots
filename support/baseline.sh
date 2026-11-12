#!/bin/bash
set -euo pipefail
source support/build.sh
compile "$R/support/baseline.cpp" "$B/baseline"
"$B/baseline"
source support/old-tests.sh
