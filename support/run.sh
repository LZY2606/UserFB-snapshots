#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) echo 'usage: run.sh baseline|target' >&2; exit 2;; esac
work=$(mktemp -d "${TMPDIR:-/tmp}/check-run.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
export CLANG_MODULE_CACHE_PATH="$work/clang-cache"
mkdir -p "$CLANG_MODULE_CACHE_PATH"
export SHM_ROOT="$work/shm"
mkdir -p "$SHM_ROOT"
cc=${CC:-cc}
compile_tool() {
  tool=$1
  shift
  "$cc" -std=gnu11 -O1 -Wno-pointer-sign -Wno-format -Wno-deprecated-declarations \
    -DBIN_PATH='"."' -DAFL_PATH='"."' -DDOC_PATH='"docs"' \
    -Dshmget=check_shmget -Dshmat=check_shmat -Dshmctl=check_shmctl \
    "$tool.c" support/file-shm.c "$@" -o "$work/$tool"
}
if [ "$mode" = target ]; then
  for src in afl-tool-runner.c afl-tool-runner.h tests/forkserver_contract.c tests/forkserver_target.c; do
    [ -s "$src" ] || { echo "missing or empty required file: $src" >&2; exit 2; }
  done
  compile_tool afl-tmin afl-tool-runner.c
  compile_tool afl-analyze afl-tool-runner.c
else
  compile_tool afl-tmin
  compile_tool afl-analyze
fi
"$cc" -std=gnu11 -O1 -Wno-pointer-sign -Dshmat=check_shmat -c llvm_mode/afl-llvm-rt.o.c -o "$work/runtime.o"
"$cc" -std=gnu11 -O1 support/baseline-target.c support/file-shm.c "$work/runtime.o" -o "$work/input-target"
printf 'Apadding' > "$work/input"
export AFL_BASELINE_START_LOG="$work/starts"
"$work/afl-tmin" -i "$work/input" -o "$work/min" -m none -t 1000 \
  -- "$work/input-target" > "$work/tmin.log" 2>&1 || { cat "$work/tmin.log"; exit 1; }
cat "$work/tmin.log"
[ "$(wc -c < "$work/min" | tr -d ' ')" = 1 ]
[ "$(od -An -tx1 "$work/min" | tr -d ' \n')" = 41 ]
starts=$(wc -l < "$work/starts" | tr -d ' ')
[ "$starts" -gt 1 ]
printf 'BASELINE_TMIN bytes=1 hex=41 starts=%s\n' "$starts"
"$work/afl-analyze" -i "$work/input" -m none -t 1000 -- "$work/input-target" \
  > "$work/analyze.log" 2>&1 || { cat "$work/analyze.log"; exit 1; }
cat "$work/analyze.log"
printf 'BASELINE_ANALYZE bytes=8 completed\n'
unset AFL_BASELINE_START_LOG
if [ "$mode" = baseline ]; then
  for app in afl-tmin afl-analyze; do
    if "$work/$app" -F -i "$work/input" -- "$work/input-target" > "$work/missing.log" 2>&1; then
      echo "unexpected: $app already accepts -F" >&2; exit 1
    fi
    grep -E 'invalid option|illegal option' "$work/missing.log"
    printf 'BASELINE_CAPABILITY %s -F unsupported\n' "$app"
  done
else
  # Both real tools must accept and execute -F before the contract binary runs.
  "$work/afl-tmin" -F -i "$work/input" -o "$work/fork-min" -m none -t 1000 \
    -- "$work/input-target"
  cmp "$work/min" "$work/fork-min"
  "$work/afl-analyze" -F -i "$work/input" -m none -t 1000 -- "$work/input-target"
  "$cc" -std=gnu11 -O1 -I. tests/forkserver_target.c support/file-shm.c "$work/runtime.o" -o "$work/contract-target"
  "$cc" -std=gnu11 -O1 -I. support/contract-main.c tests/forkserver_contract.c \
    -o "$work/contracts"
  "$work/contracts" "$work/afl-tmin" "$work/afl-analyze" "$work/contract-target" \
    > "$work/contracts.log" 2>&1 || { cat "$work/contracts.log"; exit 1; }
  cat "$work/contracts.log"
  [ "$(grep -c '^PASS ' "$work/contracts.log")" = 5 ]
  grep -qx 'CONTRACT_CASES=5' "$work/contracts.log"
fi
