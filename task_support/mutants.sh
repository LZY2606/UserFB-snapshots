#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/run.sh target
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
for kind in PreviewCommit PreemphPreview; do
  mkdir "$scratch/$kind"
  tar --exclude=.task-build --exclude=.git -cf - . | tar -xf - -C "$scratch/$kind"
  file="$scratch/$kind/amrwbenc/src/voAMRWBEnc.c"
  cp "$file" "$scratch/before.c"
  case "$kind" in
    PreviewCommit)
      sed 's/Decim_12k8(error, L_FILT16k, new_speech + L_FRAME, code);/Decim_12k8(error, L_FILT16k, new_speech + L_FRAME, st->mem_decim);/' "$scratch/before.c" > "$file" ;;
    PreemphPreview)
      sed 's/tmp = new_speech\[L_FRAME - 1\];/tmp = new_speech[L_FRAME + L_FILT - 1];/' "$scratch/before.c" > "$file" ;;
  esac
  if cmp -s "$scratch/before.c" "$file"; then echo "MUTATION_NOT_APPLIED $kind"; exit 1; fi
  set +e
  (cd "$scratch/$kind" && bash task_support/run.sh target) > "$scratch/$kind.log" 2>&1
  result=$?
  set -e
  cat "$scratch/$kind.log"
  [[ $result == 4 ]] && grep -q '^FAIL ' "$scratch/$kind.log" || { echo "MUTATION_NOT_DETECTED $kind exit=$result"; exit 1; }
  test -x "$scratch/$kind/.task-build/target"
  echo "MUTATION_CAUGHT $kind exit=4"
done
echo FRONTEND_REFERENCE_COMPLETE
