#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B .task-build/lib -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release -DSHARED_LIBRARY=OFF -DBUILD_DEMOS=OFF \
  -DWITH_C=OFF -DWITH_DOCS=OFF -DWITH_BINDING_DOCS=OFF \
  -DWITH_PYTHON=OFF -DWITH_FFMPEG=OFF -DWITH_FFTW=OFF -DWITH_JACK=OFF \
  -DWITH_LIBSNDFILE=OFF -DWITH_OPENAL=OFF -DWITH_RUBBERBAND=OFF \
  -DWITH_SDL=OFF -DWITH_COREAUDIO=OFF -DWITH_PULSEAUDIO=OFF \
  -DWITH_PIPEWIRE=OFF -DWITH_WASAPI=OFF
cmake --build .task-build/lib --target audaspace --parallel 4
c++ -std=c++17 -Iinclude -I.task-build/lib task_support/baseline.cpp \
  .task-build/lib/libaudaspace.a -pthread -o .task-build/baseline
