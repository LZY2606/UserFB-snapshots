#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s checks/target.c || { echo 'MISSING_OR_EMPTY checks/target.c' >&2; exit 2; }
b=$(mktemp -d "${TMPDIR:-/tmp}/spvm-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
cc -std=c99 -I inc -I checks src/*.c src/ext/GLSL450.c checks/target.c checks/target_main.c -lm -o "$b/target"
"$b/target" > "$b/output"
cat "$b/output"
# A target that exits(0) before all groups must fail, even if a prior group ran.
awk '
 BEGIN {names[1]="relocate";names[2]="continuation";names[3]="reject"; stage=1; total=0; done=0; bad=0}
 /^GROUP_OK / {
  if(stage>3 || NF!=3 || $2!=names[stage] || $3!~/^[1-9][0-9]*$/) bad=1;
  else {total+=$3;stage++}
 }
 /^TARGET_OK / {
  if(stage!=4 || NF!=2 || $2!~/^[1-9][0-9]*$/ || $2!=total || done) bad=1;
  done=1
 }
 END {if(bad || stage!=4 || !done) exit 1}
' "$b/output"
test -s examples/checkpoint.c || { echo 'MISSING_OR_EMPTY examples/checkpoint.c' >&2; exit 2; }
cc -std=c99 -I inc -I checks src/*.c src/ext/GLSL450.c examples/checkpoint.c -lm -o "$b/example"
"$b/example" > "$b/example-output"
cat "$b/example-output"
test "$(grep -c '^CHECKPOINT_EXAMPLE_OK$' "$b/example-output")" = 1
bash checks/baseline.sh
printf 'CHECKPOINT_TARGET_COMPLETE\n'
