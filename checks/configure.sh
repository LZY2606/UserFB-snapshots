#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Release -Dozz_build_postfix=OFF -Dozz_build_tools=OFF -Dozz_build_fbx=OFF -Dozz_build_gltf=OFF -Dozz_build_samples=OFF -Dozz_build_howtos=OFF -Dozz_build_tests=ON -DBUILD_SHARED_LIBS=OFF
cmake --build build-check --parallel 4 --target inertial_baseline test_sampling_job test_blending_job test_local_to_model_job
