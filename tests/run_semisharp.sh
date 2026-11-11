#!/usr/bin/env bash
# Build and run the semi-sharp crease subdivision tests.
#
# Configures a Debug build in a temporary directory using the Eigen and
# GoogleTest copies bundled in the repository (viewers and examples are
# disabled), then runs the existing SubdivisionTest suite together with
# the new SemiSharpTest suite.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/pmp-semisharp-build.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT

cmake -S "${repo_root}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPMP_BUILD_TESTS=ON \
    -DPMP_BUILD_EXAMPLES=OFF \
    -DPMP_BUILD_VIEWERS=OFF \
    -DPMP_INSTALL=OFF

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
cmake --build "${build_dir}" --target gtest_runner -j "${jobs}"

"${build_dir}/tests/gtest_runner" \
    --gtest_filter='SubdivisionTest.*:SemiSharpTest.*'

echo "semisharp subdivision PASS"
