#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s regress/paged_atlas.c || { echo 'missing or empty regress/paged_atlas.c' >&2; exit 2; }
d=$(mktemp -d "${TMPDIR:-/tmp}/fontstash-paged.XXXXXX")
trap 'rm -rf "$d"' EXIT HUP INT TERM
cc -std=c99 -O2 -Isrc -Iharness regress/paged_atlas.c harness/target_main.c -lm -o "$d/paged"
"$d/paged"
sh harness/baseline.sh
test -s example/paged_headless.c || { echo 'missing or empty example/paged_headless.c' >&2; exit 2; }
cc -std=c99 -O2 -Isrc -Iharness example/paged_headless.c -lm -o "$d/demo"
"$d/demo" "$d/paged.pgm"
test -s "$d/paged.pgm"
awk 'NR==1 {if ($0!="P2") exit 1; next} NR==2 {if (NF!=2 || $1<1 || $2<1) exit 1; expected=$1*$2;next} NR==3 {if ($0!="255") exit 1;next} NR>3 {for(i=1;i<=NF;i++){if($i!~/^[0-9]+$/ || $i>255)exit 1; n++;if($i>0)ink++}} END {if(expected<1 || n!=expected || !ink)exit 1}' "$d/paged.pgm"
echo PAGED_PGM_OK
