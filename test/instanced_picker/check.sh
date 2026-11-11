#!/usr/bin/env bash
# Builds and runs the windowless instanced picking example using the system
# C++20 compiler. No dependencies are downloaded: bvh/v2 is header-only and
# lives in this repository.
set -euo pipefail

# Resolve repository paths relative to this script so it runs from anywhere.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

cxx="${CXX:-c++}"
build_dir="${TMPDIR:-/tmp}/instanced_picker_check"
mkdir -p "${build_dir}"
binary="${build_dir}/instanced_picker_test"

echo "Compiler: $("${cxx}" --version | head -n 1)"
echo "Repository: ${repo_root}"
echo "Build dir: ${build_dir}"

"${cxx}" -std=c++20 -O2 -Wall -Wextra -pedantic \
    -DINSTANCED_PICKER_ENABLE_TEST_HOOKS \
    -I"${repo_root}/src" \
    -I"${repo_root}/examples" \
    "${script_dir}/instanced_picker_test.cpp" \
    -o "${binary}"

"${binary}"
