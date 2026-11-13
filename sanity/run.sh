set -euo pipefail
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) echo "unknown mode" >&2; exit 2 ;; esac
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -I. -c fuzzy.c -o "$b/fuzzy.o"
cc -std=c99 -O2 -I. -c edit_dist.c -o "$b/edit.o"
if [ "$mode" = target ]; then
  for source in fuzzy_edit.h fuzzy_edit.c sanity/edit_cases.c; do
    test -s "$source" || { echo "missing or empty: $source" >&2; exit 2; }
  done
  cc -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -I. -Isanity sanity/harness.c sanity/edit_cases.c fuzzy_edit.c "$b/fuzzy.o" "$b/edit.o" -o "$b/target"
  "$b/target" >"$b/target.out"
  cat "$b/target.out"
  bash sanity/check-output.sh "$b/target.out"
fi
cc -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -I. sanity/baseline.c "$b/fuzzy.o" "$b/edit.o" -o "$b/baseline"
"$b/baseline"
cc -std=c99 -O2 -D__UNITTEST edit_dist.c -o "$b/edit_test"
"$b/edit_test"
printf '%s\n' SSDEEP_EDIT_TEST_OK
