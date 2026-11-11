#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/texture_remeshing_test.cpp
cmake -S checks -B build-check -DCMAKE_BUILD_TYPE=Debug
cmake --build build-check --target gtest_runner -j 4
runner="$PWD/build-check/pmp-build/tests/gtest_runner"
cases=(continuous_transfer seam_refinement coarsen_and_flip chart_projection feature_selection obj_roundtrip)
listed=$("$runner" --gtest_filter='TextureRemeshingTest.*' --gtest_list_tests)
for name in "${cases[@]}"; do grep -Fx "  $name" <<< "$listed" >/dev/null; done
out=$(cd build-check/pmp-build/tests && ./gtest_runner --gtest_filter='TextureRemeshingTest.*:RemeshingTest.*:SurfaceMeshTest.*' 2>&1)
printf '%s\n' "$out"
for name in "${cases[@]}"; do
    grep -F "[ RUN      ] TextureRemeshingTest.$name" <<< "$out" >/dev/null
    grep -F "[       OK ] TextureRemeshingTest.$name " <<< "$out" >/dev/null
done
if grep -F '[  SKIPPED ]' <<< "$out" >/dev/null; then exit 1; fi
grep -E '\[  PASSED  \] [1-9][0-9]* tests?\.' <<< "$out" >/dev/null
printf 'PMP_TEXTURE_REMESH_TARGET_OK\n'
