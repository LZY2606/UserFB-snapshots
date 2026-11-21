#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cc -D__FILENAME__=__FILE__ -std=c99 -O2 -Itask_support/uuid_compat -Ideps/uuid4/src -c task_support/uuid_compat.c -o .task-build/uuid_compat.o
cc -D__FILENAME__=__FILE__ -std=c99 -O2 -Ideps/uuid4/src -c deps/uuid4/src/uuid4.c -o .task-build/uuid4.o
ar rcs .task-build/libtaskuuid.a .task-build/uuid_compat.o .task-build/uuid4.o
cmake -S . -B .task-build/cmake -DBUILD_SHARED_LIBS=OFF -DLIBMXF_BUILD_LIB_ONLY=ON -DLIBMXF_BUILD_TESTING=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Duuid_link_lib="$PWD/.task-build/libtaskuuid.a" -DCMAKE_C_FLAGS="-I$PWD/task_support/uuid_compat"
cmake --build .task-build/cmake --target MXF --parallel 2
cc -D__FILENAME__=__FILE__ -std=c99 -O2 -I. -I.task-build/cmake -Dmain=upstream_essence_main -c test/test_essencecontainer.c -o .task-build/essence-test.o
cc -D__FILENAME__=__FILE__ -std=c99 -O2 -I. task_support/baseline.c .task-build/essence-test.o .task-build/cmake/mxf/libMXF.a .task-build/libtaskuuid.a -lm -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
  shopt -s nullglob
  sources=(examples/pcmwrap/*.c)
  if ((${#sources[@]})); then
    for source in "${sources[@]}"; do
      [[ $source == examples/pcmwrap/main.c ]] && continue
      cc -D__FILENAME__=__FILE__ -std=c99 -O2 -I. -I.task-build/cmake -c "$source" -o ".task-build/pcmwrap-$(basename "$source" .c).o"
    done
    if [[ -f examples/pcmwrap/main.c ]]; then
      cc -D__FILENAME__=__FILE__ -std=c99 -O2 -I. -I.task-build/cmake examples/pcmwrap/main.c .task-build/pcmwrap-*.o .task-build/cmake/mxf/libMXF.a .task-build/libtaskuuid.a -lm -o .task-build/pcmwrap
    fi
  fi
fi
