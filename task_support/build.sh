#!/bin/bash
set -euo pipefail
mkdir -p .task-build
flags=(-std=c++11 -O0 -pthread -DGLM_FORCE_PURE -D_MSC_EXTENSIONS -include cfloat -include limits -include atomic -I. -Isrc -Ithird_party/glm -Ithird_party/stb/include -Ithird_party/md5)
cc -std=c99 -O0 -Ithird_party/md5 -c third_party/md5/md5.c -o .task-build/md5.o
objects=(.task-build/md5.o)
for source in src/Base/Geometry.cpp src/Base/Logger.cpp src/Base/ImageUtils.cpp src/Render/Software/*.cpp; do
 object=".task-build/$(basename "${source%.cpp}").o"
 c++ "${flags[@]}" -c "$source" -o "$object"
 objects+=("$object")
done
c++ "${flags[@]}" task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
