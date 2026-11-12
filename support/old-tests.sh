#!/bin/bash
set -euo pipefail
# Called with R and B from build.sh, uses exact upstream source, fail on false.
c++ -std=c++17 -DHAVE_CONFIG_H -I"$B" -I"$B/libde265" -I"$R" -I"$R/support" \
 "$R/support/old_cabac_main.cpp" "$R/support/upstream/test-cabac.cc" \
 "$B/libde265/libde265.a" -lpthread -o "$B/upstream-cabac"
"$B/upstream-cabac"
