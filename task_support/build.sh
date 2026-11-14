#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-build
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DTASK_NLSF_TEST -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_TESTING=ON -DOPUS_BUILD_PROGRAMS=OFF \
  -DOPUS_ENABLE_FLOAT_API=ON -DOPUS_DISABLE_INTRINSICS=ON -DOPUS_DRED=OFF -DOPUS_OSCE=OFF \
  -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF \
  > .task-build/configure.log 2>&1 || { tail -35 .task-build/configure.log >&2; exit 1; }
cmake --build .task-build --target opus test_opus_padding -j 4 \
  > .task-build/build.log 2>&1 || { tail -40 .task-build/build.log >&2; exit 1; }
${CC:-cc} -std=c99 -O2 -Iinclude task_support/baseline.c .task-build/libopus.a -lm -o .task-build/baseline
${CC:-cc} -std=c99 -O2 -Iinclude task_support/legacy_projection.c .task-build/libopus.a -lm -o .task-build/legacy_projection
