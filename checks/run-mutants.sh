#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/render_regression.c
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT
# All compilation precedes execution, so build failures are never detection.
for variant in normal loop_neighbor filter_reset; do
  mkdir "$work_dir/$variant"
  tar --exclude=.git --exclude=.task-build -cf - . | tar -xf - -C "$work_dir/$variant"
  cd "$work_dir/$variant"
  case "$variant" in
    loop_neighbor)
      sed 's/(pos >= tmpLoopEnd \&\& isLooping ? tmpLoopStart : pos + 1)/(pos + 1)/g' tsf.h > tsf.new
      mv tsf.new tsf.h
      ;;
    filter_reset)
      sed '/numSamples -= blockSamples;/a\
        tmpLowpass.z1 = tmpLowpass.z2 = 0;' tsf.h > tsf.new
      mv tsf.new tsf.h
      ;;
  esac
  bash checks/build-target.sh > build.log 2>&1 || { tail -n 12 build.log; echo "BUILD_FAILED $variant"; exit 1; }
  echo "BUILD_OK $variant"
  set +e
  bash task_support/run.sh target > run.log 2>&1
  status=$?
  set -e
  if [[ "$variant" == normal ]]; then
    [[ "$status" == 0 ]] || { tail -n 12 run.log; exit 1; }
    echo NORMAL_CONTENT_OK
  else
    [[ "$status" == 4 ]] && grep -q '^FAIL ' run.log || { tail -n 12 run.log; echo "INVALID_DETECTION $variant status=$status"; exit 1; }
    grep '^FAIL ' run.log | head -n 1
    echo "DETECTED $variant build=0 content=1"
  fi
  cd - >/dev/null
done
echo MUTANTS_COMPLETE
