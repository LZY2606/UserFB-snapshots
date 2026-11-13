#!/usr/bin/env bash
set -euo pipefail
: "${TASK_BUILD:?}"
cmake -S . -B "$TASK_BUILD/cmake" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS=-U__ARM_NEON__ -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DLOTTIE_MODULE=OFF -DLOTTIE_THREAD=OFF -DLOTTIE_TEST=OFF -DLOTTIE_CCACHE=OFF -DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON > "$TASK_BUILD/configure.log" 2>&1 || { cat "$TASK_BUILD/configure.log"; exit 1; }
cmake --build "$TASK_BUILD/cmake" --target rlottie -j 4 > "$TASK_BUILD/compile.log" 2>&1 || { cat "$TASK_BUILD/compile.log"; exit 1; }
