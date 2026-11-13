#!/bin/bash
set -euo pipefail
source support/build.sh
compile "$R/support/sidx_roundtrip.cpp" "$B/sidx_roundtrip"
"$B/sidx_roundtrip"
compile "$R/support/processor_baseline.cpp" "$B/processor_baseline"
"$B/processor_baseline" "${1:-witness}"
