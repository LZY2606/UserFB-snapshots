#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cxx=${CXX:-c++}
flags=(-std=c++17 -O0 -DNDEBUG -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -I. -Imodules -pthread)
platform=()
extension=cpp
if [[ $(uname -s) == Darwin ]]; then
  extension=mm
  platform=(-framework Cocoa -framework Foundation -framework IOKit -framework Security -framework Accelerate)
else
  platform=(-ldl -lrt)
fi
for module in juce_core juce_events juce_audio_basics; do
 source_file=modules/${module}/${module}.${extension}
 if [[ ! -f .task-build/${module}.o || $source_file -nt .task-build/${module}.o || $(find modules/${module} -type f -newer .task-build/${module}.o | head -c 1) ]]; then
  "$cxx" "${flags[@]}" -c "$source_file" -o .task-build/${module}.o
 fi
done
"$cxx" "${flags[@]}" -c task_support/transport.cpp -o .task-build/transport.o
shopt -s nullglob
extras=(task_src/*.cpp)
objects=(.task-build/juce_core.o .task-build/juce_events.o .task-build/juce_audio_basics.o .task-build/transport.o)
if [[ ${#extras[@]} -gt 0 ]]; then
 for source_file in "${extras[@]}"; do
  object_file=.task-build/extra_$(basename "$source_file" .cpp).o
  "$cxx" "${flags[@]}" -c "$source_file" -o "$object_file"
  objects+=("$object_file")
 done
fi
"$cxx" "${flags[@]}" task_support/baseline.cpp "${objects[@]}" "${platform[@]}" -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
 "$cxx" "${flags[@]}" -x c++ task_support/driver.c task_tests/test_source_edits.cpp -x none "${objects[@]}" "${platform[@]}" -o .task-build/target
fi
