#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $# -gt 0 ]]; then shift; fi
case "$mode" in baseline|target) ;; *) echo 'usage: bash checks/run.sh baseline|target [--seed N --case N --group G]' >&2; exit 2;; esac
if [[ "$mode" == baseline && $# -ne 0 ]]; then exit 2; fi
b=$(mktemp -d "${TMPDIR:-/tmp}/onig-budget.XXXXXX")
log_root=${BUDGET_LOG_DIR:-.budget-logs}
mkdir -p "$log_root"
log=$(cd "$log_root" && pwd)/$(basename "$b")
mkdir -p "$log"
trap 'rm -rf "$b"' EXIT
printf 'BUILD_LOG_DIR=%s\n' "$log"
step() {
  local label=$1
  shift
  local rc=0
  "$@" >"$log/$label.stdout" 2>"$log/$label.stderr" || rc=$?
  printf '%s\n' "$rc" >"$log/$label.exit"
  if [[ "$rc" != 0 ]]; then cat "$log/$label.stdout" "$log/$label.stderr" >&2; return "$rc"; fi
}
step configure cmake -S . -B "$b/build" -DBUILD_SHARED_LIBS=OFF -DBUILD_TEST=ON -DINSTALL_DOCUMENTATION=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
step build cmake --build "$b/build" --target onig test_syntax -j 4
step witness-compile cc -std=c11 -DONIG_STATIC -I. -Isrc -I"$b/build" checks/witness.c "$b/build/libonig.a" -o "$b/base"
step witness "$b/base"
cat "$log/witness.stdout"
step syntax "$b/build/test/test_syntax"
tail -3 "$log/syntax.stdout"
echo ONIG_EXISTING_SYNTAX_OK
if [[ "$mode" == target ]]; then
  for f in test/redos_budget.c test/redos_mutations.sh docs/redos_budget.md; do
    [[ -s "$f" ]] || { echo "MISSING_TARGET $f" >&2; exit 1; }
  done
  sources=(test/redos_budget.c)
  if [[ -f test/redos_model.c ]]; then sources+=(test/redos_model.c); fi
  step target-compile cc -std=c11 -DONIG_STATIC -I. -Isrc -I"$b/build" checks/budget_driver.c "${sources[@]}" "$b/build/libonig.a" -o "$b/target"
  rc=0
  "$b/target" "$@" >"$log/target.stdout" 2>"$log/target.stderr" || rc=$?
  printf '%s\n' "$rc" >"$log/target.exit"
  cat "$log/target.stdout" "$log/target.stderr"
  [[ "$rc" == 0 ]] || exit "$rc"
  if grep -Eiq '(^|[^[:alnum:]_])skip([:[:space:]]|$)' "$log/target.stdout" "$log/target.stderr"; then echo TARGET_SKIP >&2; exit 1; fi
  if [[ $# == 0 ]]; then
    for group in corpus search calls reuse; do
      grep -Eq "^GROUP_OK $group cases=24 checks=[1-9][0-9]*$" "$log/target.stdout"
      [[ $(grep -c "^CASE_OK $group " "$log/target.stdout") == 24 ]]
    done
    grep -Eq '^ONIG_BUDGET_TARGET_OK groups=4 checks=[1-9][0-9]*$' "$log/target.stdout"
  else
    grep -Eq '^ONIG_BUDGET_TARGET_OK groups=[1-4] checks=[1-9][0-9]*$' "$log/target.stdout"
  fi
fi
