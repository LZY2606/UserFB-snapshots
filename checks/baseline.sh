#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-.}/opensubdiv-baseline.XXXXXX")
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DNO_EXAMPLES=ON -DNO_TUTORIALS=ON -DNO_REGRESSION=ON -DNO_DOC=ON -DNO_OMP=ON -DNO_TBB=ON -DNO_CUDA=ON -DNO_OPENCL=ON -DNO_OPENGL=ON -DNO_METAL=ON -DNO_PTEX=ON -DNO_TESTS=ON -DNO_MACOS_FRAMEWORK=ON -DBUILD_SHARED_LIBS=OFF -DOPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES=ON
cmake --build "$b" --target osd_static_cpu -j 4
c++ -std=c++17 -I . checks/baseline.cpp "$b/lib/libosdCPU.a" -o "$b/baseline"
"$b/baseline"
if [[ ${1:-} != --cube-only ]]; then
  c++ -std=c++17 -I . checks/loop_witness.cpp "$b/lib/libosdCPU.a" -o "$b/loop_witness"
  "$b/loop_witness"
fi
