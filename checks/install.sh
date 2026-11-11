#!/usr/bin/env bash
set -euo pipefail
mkdir -p .deps
if [ ! -f .deps/eigen/Eigen/Core ]; then
  a=$(mktemp)
  trap 'rm -f "$a"' EXIT
  curl -fL --retry 2 https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz -o "$a"
  printf '%s  %s\n' 8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72 "$a" | shasum -a 256 -c -
  tar -xzf "$a" -C .deps
  mv .deps/eigen-3.4.0 .deps/eigen
fi
test -s .deps/eigen/Eigen/Core
echo EIGEN_3_4_0_READY
