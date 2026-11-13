set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source task_support/build.sh
build_target "$build/normal"
for group in planes spectrum frames; do
  "$build/normal/target" "$group" > "$build/$group.log" 2>&1
  cat "$build/$group.log"
  grep -Eq "^TASK_GROUP $group checks=[1-9][0-9]* complete$" "$build/$group.log"
done
for mode in context lsb-sign; do
  copy="$build/$mode"
  mkdir -p "$copy"
  cp -R src include task_support "$copy/"
  awk -v mode="$mode" -f task_support/mutate.awk src/spec.c > "$copy/src/spec.c"
  diff -u src/spec.c "$copy/src/spec.c" > "$build/$mode.diff" || test "$?" = 1
  cat "$build/$mode.diff"
  (cd "$copy"; build_target "$build/$mode-build")
  printf 'MUTANT_BUILD_OK %s\n' "$mode"
  set +e
  "$build/$mode-build/target" spectrum > "$build/$mode.log" 2>&1
  status=$?
  set -e
  cat "$build/$mode.log"
  test "$status" = 2
  grep -q '^LC3_ENTROPY_MISMATCH ' "$build/$mode.log"
  printf 'MUTANT_CONTENT_CAUGHT %s\n' "$mode"
done
bash task_support/baseline.sh
printf 'LC3_ENTROPY_SUITE_COMPLETE groups=3 mutants=2\n'
