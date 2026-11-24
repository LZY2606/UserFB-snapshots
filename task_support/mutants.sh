#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
origin=$PWD
logs="$origin/task_support/results"
mkdir -p "$logs"
bash task_support/run.sh target > "$logs/normal.log" 2>&1 || {
  tail -30 "$logs/normal.log" >&2
  exit 1
}
printf 'NORMAL_MS_TIMELINE_PASS\n'
trial_root=$(mktemp -d)
trap 'rm -rf "$trial_root"' EXIT
for defect in fec_flag right_channel; do
  trial="$trial_root/$defect"
  mkdir "$trial"
  tar --exclude=.git --exclude=.task-build --exclude=task_support/results -cf - . | tar -xf - -C "$trial"
  (
    cd "$trial"
    patch -p1 < "$origin/task_support/defects/$defect.patch"
    bash task_support/build.sh
  ) > "$logs/$defect-build.log" 2>&1 || {
    tail -30 "$logs/$defect-build.log" >&2
    printf 'MUTANT_BUILD_FAILED %s\n' "$defect" >&2
    exit 1
  }
  set +e
  (cd "$trial" && bash task_support/run.sh target) > "$logs/$defect-run.log" 2>&1
  status=$?
  set -e
  # The shared driver returns 4 on case failure; require its content assertion.
  if [[ $status != 4 ]] || ! grep -Eq '^FAIL tests/ms_timeline\.c:[0-9]+ ' "$logs/$defect-run.log"; then
    tail -25 "$logs/$defect-run.log" >&2
    printf 'MUTANT_NOT_CONTENT_DETECTED %s exit=%s\n' "$defect" "$status" >&2
    exit 1
  fi
  printf 'DETECTED_%s\n' "$defect"
done
printf 'MS_TIMELINE_MUTANTS_COMPLETE\n'
