#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
command -v cc >/dev/null
command -v c++ >/dev/null
mkdir -p .deps
archive=.deps/googletest.tar.gz
if [ ! -f "$archive" ]; then
  curl -fsSL --retry 2 https://codeload.github.com/google/googletest/tar.gz/b514bdc898e2951020cbdca1304b75f5950d1f59 -o "$archive"
fi
printf '%s  %s\n' 9257316d65d6259f6596bc4fa5464092305ab3f3d9dc6d7e5780ab462d8baa64 "$archive" | shasum -a 256 -c -
if [ ! -f .deps/googletest/googletest/src/gtest-all.cc ]; then
  mkdir -p .deps/googletest
  tar -xzf "$archive" --strip-components=1 -C .deps/googletest
fi
printf 'DEPENDENCY googletest 1.15.2 b514bdc898e2951020cbdca1304b75f5950d1f59\n'
