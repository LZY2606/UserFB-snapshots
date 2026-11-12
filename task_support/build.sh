#!/bin/bash
set -euo pipefail
: "${task_build:?task_build must name a temporary output directory}"
c++ -std=c++11 -O2 -fno-strict-aliasing -DSUPPORT_BC7E=0 -I . -I task_support "$@" rgbcx.cpp bc7enc.cpp bc7decomp.cpp bc7decomp_ref.cpp ert.cpp rdo_bc_encoder.cpp utils.cpp lodepng.cpp -o "$task_build/run"
