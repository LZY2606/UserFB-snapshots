#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/build.XXXXXX")
trap 'rm -rf "$work"' EXIT
export LC_ALL=C
c++ -std=c++17 -O2 -DNO_OMP -I src -I task_support tests/mlt_transport.cpp task_support/driver.cpp  -o "$work/target"
set +e
"$work/target" > "$work/stdout" 2> "$work/stderr"
status=$?
set -e
cat "$work/stdout"
cat "$work/stderr" >&2
if [ "$status" -ne 0 ]; then exit "$status"; fi
awk '
BEGIN { n=split("replay chain transport",names," "); phase=0; bad=0; }
/^BEGIN / { if(phase%2!=0 || $2!=names[int(phase/2)+1]) bad=1; phase++; next; }
/^DONE / { if(phase%2!=1 || $2!=names[int(phase/2)+1] || $3!~/^[0-9]+$/ || $3+0<1) bad=1; phase++; next; }
/^ALL_DONE$/ { if(phase!=2*n) bad=1; final++; next; }
END { if(bad || phase!=2*n || final!=1) exit 31; }
' "$work/stdout"
bash task_support/baseline.sh
