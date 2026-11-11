#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
./build-check/checks/inertial_baseline
ctest --test-dir build-check --no-tests=error --output-on-failure -R '^(test_sampling_job|test_blending_job|test_local_to_model_job)$'
