#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
if test -f Cpp/Source/SphereBvh.cpp; then c++ -std=c++11 -O2 -I. -ICpp/Source -Isrc/renderer -c Cpp/Source/SphereBvh.cpp -o "$b/newmodule.o"; else c++ -x c++ -c /dev/null -o "$b/newmodule.o"; fi
c++ -std=c++11 -O2 -pthread -ICpp/Source -I. -Isupport support/target_main.cpp tests/sphere_bvh_cases.cpp Cpp/Source/Test.cpp Cpp/Source/Maths.cpp Cpp/Source/enkiTS/TaskScheduler.cpp Cpp/Source/enkiTS/TaskScheduler_c.cpp "$b/newmodule.o" -o "$b/target"
"$b/target"
