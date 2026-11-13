#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DBUILD_SHARED_LIBS=OFF -DBUILD_APPS=OFF -DBUILD_TESTING=OFF -DBUILD_SHAPELIB_CONTRIB=OFF
cmake --build "$b" --target shp -j 2
cc -std=c99 -UNDEBUG -I. devcheck/baseline.c "$b/libshp.a" -lm -o "$b/baseline"
"$b/baseline" "$b"
if [[ ${1:-baseline} == target ]]; then
 cc -std=c99 -UNDEBUG -I. -Idevcheck devcheck/target-main.c tests/test_polygon_components.c "$b/libshp.a" -lm -o "$b/target"
 "$b/target" > "$b/target.log"
 cat "$b/target.log"
 grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$b/target.log"
fi
