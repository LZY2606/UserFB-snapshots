#!/bin/sh
set -eu
command -v "${CC:-cc}"
command -v "${CXX:-c++}"
echo TOOLCHAIN_ONLY_NO_DOWNLOAD
