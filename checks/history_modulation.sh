#!/usr/bin/env bash
#
# History matched modulation check.
#
# Builds the DSPFilters library and the history modulation test in a temporary
# directory, then runs:
#   1. history_modulation      - the new independent-reference conformance test
#   2. baseline                - the retained legacy check, kept to demonstrate
#                                the realization-dependent modulation difference
#                                that history matched modulation removes
#   3. history sweep example   - a small C++ automation example
#
set -euo pipefail

# Locate the repository root from this script's own directory.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
inc="${repo_root}/shared/DSPFilters/include"
src="${repo_root}/shared/DSPFilters/source"

# Honour TMPDIR (and fall back to /tmp) with an explicit mktemp template so the
# build works even when the platform default temp location is not writable.
tmp_root="${TMPDIR:-/tmp}"
build_dir="$(mktemp -d "${tmp_root}/history_modulation.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT

cxx="${CXX:-c++}"
common_flags=(-std=c++11 -O2 -Wall -I"${inc}")
library_sources=("${src}/"*.cpp)

echo "Building in ${build_dir}"

echo "[1/3] history modulation conformance test"
"${cxx}" "${common_flags[@]}" \
  "${repo_root}/checks/history_modulation.cpp" \
  ${library_sources[@]} \
  -o "${build_dir}/history_modulation"
"${build_dir}/history_modulation"

echo
echo "[2/3] retained legacy baseline (shows the modulation difference)"
"${cxx}" "${common_flags[@]}" \
  "${repo_root}/checks/baseline.cpp" \
  ${library_sources[@]} \
  -o "${build_dir}/baseline"
"${build_dir}/baseline"

echo
echo "[3/3] C++ parameter sweep example"
"${cxx}" "${common_flags[@]}" \
  "${repo_root}/checks/history_modulation_sweep_example.cpp" \
  ${library_sources[@]} \
  -o "${build_dir}/sweep_example"
"${build_dir}/sweep_example"
