#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
mkdir "$b/obj"
for f in src/*.c src/spss/*.c; do
 case "$f" in *readstat_zsav*) continue;; esac
 cc -std=c99 -O0 -Isrc -c "$f" -o "$b/obj/$(basename "$f" .c).o"
done
ar rcs "$b/libreadstat.a" "$b"/obj/*.o
INCLUDES="-Isrc"
LIBS="$b/libreadstat.a -liconv -lm"
cc $INCLUDES sanity/baseline.c $LIBS -o "$b/baseline"
echo READSTAT_BUILD_OK
case "${1:-baseline}" in
baseline) "$b/baseline" ;;
target) test -s sanity/target.c
cc ${CFLAGS_EXTRA:-} -Isanity $INCLUDES sanity/main.c sanity/target.c $LIBS -o "$b/target"
 "$b/target"
 "$b/baseline" ;;
*) exit 2;;
esac
