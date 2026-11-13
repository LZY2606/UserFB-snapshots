#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
mode=${1:-baseline}
case "$mode" in
baseline)
    ${CC:-cc} -std=c99 -O2 -Wall -Wextra -I src -I support support/baseline.c -lm -o build/baseline
    build/baseline
    ${CXX:-c++} -std=c++11 -O2 -I src test/test.cpp -lm -o build/author-upstream
    build/author-upstream
    ;;
target)
    test -f src/jc_voronoi_periodic.h
    test -f support/periodic_test.c
    test -f examples/periodic_texture.c
    ${CC:-cc} -std=c99 -O2 -Wall -Wextra -I src -I support -include support/config.h support/periodic_test.c -lm -o build/periodic-test
    build/periodic-test
    ${CC:-cc} -std=c99 -O2 -Wall -Wextra -I src -I support -include support/config.h examples/periodic_texture.c -lm -o build/periodic-texture
    mkdir -p build/texture
    build/periodic-texture build/texture
    test -s build/texture/texture.svg
    test -s build/texture/cells.csv
    ;;
*) printf 'unknown mode: %s\n' "$mode" >&2; exit 2;;
esac
