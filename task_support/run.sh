#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/excitation_reference.c; fi
mkdir -p .task-build
flags=(-std=c99 -O2 -Wno-deprecated-non-prototype -Wno-comment -I. -Iinclude -Isrc )
sources=(src/adaptativeCodebookSearch.c src/codebooks.c src/computeAdaptativeCodebookGain.c src/computeLP.c src/computeWeightedSpeech.c src/decodeAdaptativeCodeVector.c src/decodeFixedCodeVector.c src/decodeGains.c src/decodeLSP.c src/decoder.c src/encoder.c src/findOpenLoopPitchDelay.c src/fixedCodebookSearch.c src/gainQuantization.c src/interpolateqLSP.c src/LP2LSPConversion.c src/LPSynthesisFilter.c src/LSPQuantization.c src/postFilter.c src/postProcessing.c src/preProcessing.c src/qLSP2LP.c src/utils.c src/cng.c src/dtx.c src/vad.c)
objects=()
for src in "${sources[@]}"; do
 obj=".task-build/$(basename "${src%.c}").o"
 cc "${flags[@]}" -c "$src" -o "$obj"
 objects+=("$obj")
done
ar rcs .task-build/library.a "${objects[@]}"
if [[ $mode == target ]]; then
  cc "${flags[@]}" task_support/driver.c tests/excitation_reference.c .task-build/library.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pitch innovation frames; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc "${flags[@]}" task_support/baseline.c .task-build/library.a -lm -o .task-build/baseline
.task-build/baseline
