#!/bin/sh
set -eu
mkdir -p .task-build
${CXX:-c++} -std=c++11 -O2 -I. -Ivendor/linear/include task_support/legacy.cpp -o .task-build/legacy
