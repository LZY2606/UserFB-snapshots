#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s tests/avc_descriptions.c || { echo 'missing tests/avc_descriptions.c' >&2; exit 2; }
build=$(mktemp -d "$PWD/.check-target.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc -std=gnu99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. -Itask_support task_support/target_driver.c task_support/implementation.c tests/avc_descriptions.c -o "$build/target"
set +e
ASAN_OPTIONS=detect_leaks=0 "$build/target" >"$build/stdout" 2>"$build/stderr"
status=$?
set -e
cat "$build/stdout"
cat "$build/stderr" >&2
printf 'TARGET_PROCESS_EXIT %s\n' "$status" >&2
test "$status" -eq 0 || exit "$status"
awk '
/^GROUP (mux|demux|continuation|invalid) cases=[1-9][0-9]* checks=[1-9][0-9]* complete$/ { if (seen[$2]++) bad=1; groups++ }
/^MINIMP4_DESCRIPTIONS_COMPLETE groups=4$/ { final++ }
END { exit !(groups==4 && final==1 && !bad && seen["mux"]==1 && seen["demux"]==1 && seen["continuation"]==1 && seen["invalid"]==1) }
' "$build/stdout"
bash task_support/baseline.sh
