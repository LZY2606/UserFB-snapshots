#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh support/build.sh support/baseline.cpp
.build/check
