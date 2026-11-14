#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/mpts_cases.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -DHAVE_STDINT_H=1 -include inttypes.h -I. -c libmpegts.c -o .task-build/libmpegts.o
for src in crc/crc.c atsc/atsc.c cablelabs/cablelabs.c dvb/dvb.c hdmv/hdmv.c smpte/smpte.c; do
  obj=${src//\//_}; cc -std=c99 -O2 -DHAVE_STDINT_H=1 -include inttypes.h -I. -c "$src" -o ".task-build/${obj%.c}.o"
done
ar rcs .task-build/libmpegts.a .task-build/*.o
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -DHAVE_STDINT_H=1 -include inttypes.h -I. task_support/driver.c tests/mpts_cases.c .task-build/libmpegts.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in mpts_tables mpts_clocks mpts_media; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -O2 -DHAVE_STDINT_H=1 -include inttypes.h -I. task_support/baseline.c .task-build/libmpegts.a -lm -o .task-build/baseline
.task-build/baseline
