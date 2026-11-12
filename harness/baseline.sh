#!/bin/bash
set -euo pipefail
cmake -S . -B .build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSUPPORT_CONSOLE_APP=OFF -DBUILD_SHARED_LIB=OFF -DSUPPORT_LOCALIZATIONS=OFF >/dev/null
cmake --build .build -j 4 >/dev/null
cc -std=c99 -UNDEBUG -I include harness/baseline.c .build/libtidy.a -o .build/baseline
.build/baseline
