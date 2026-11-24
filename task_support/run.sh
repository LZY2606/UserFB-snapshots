#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/ltp_candidates.c; fi
mkdir -p .task-build
rm -f .task-build/*.o
for source in libs/srla_encoder/src/*.c libs/srla_decoder/src/*.c libs/srla_coder/src/*.c libs/srla_internal/src/*.c libs/bit_stream/src/*.c libs/static_huffman/src/*.c libs/lpc/src/*.c libs/fft/src/*.c; do
  cc -std=c99 -O1 -I. -Iinclude -Ilibs/srla_encoder/include -Ilibs/srla_decoder/include -Ilibs/srla_coder/include -Ilibs/srla_internal/include -Ilibs/bit_stream/include -Ilibs/static_huffman/include -Ilibs/lpc/include -Ilibs/fft/include -Ilibs/byte_array/include -c "$source" -o ".task-build/$(basename "$source" .c).o"
done
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -I. -Iinclude -Ilibs/srla_encoder/include -Ilibs/srla_decoder/include -Ilibs/srla_coder/include -Ilibs/srla_internal/include -Ilibs/bit_stream/include -Ilibs/static_huffman/include -Ilibs/lpc/include -Ilibs/fft/include -Ilibs/byte_array/include tests/ltp_candidates.c task_support/driver.c .task-build/*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in fitting selection integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -O1 -I. -Iinclude -Ilibs/srla_encoder/include -Ilibs/srla_decoder/include -Ilibs/srla_coder/include -Ilibs/srla_internal/include -Ilibs/bit_stream/include -Ilibs/static_huffman/include -Ilibs/lpc/include -Ilibs/fft/include -Ilibs/byte_array/include task_support/baseline.c .task-build/*.o -lm -o .task-build/baseline
.task-build/baseline
