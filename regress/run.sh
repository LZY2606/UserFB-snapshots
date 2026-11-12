#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
INCLUDES="-std=c++17 -Iinclude"
LIBS=""
c++ $INCLUDES regress/baseline.cpp -o "$b/baseline"
c++ $INCLUDES -Itest/thirdparty -DCATCH_CONFIG_MAIN test/jsonpath/src/jsonpath_expression_tests.cpp -o "$b/old"
"$b/old"
case "${1:-baseline}" in
baseline) "$b/baseline" ;;
target) test -s regress/target.cpp
c++ ${CFLAGS_EXTRA:-} -Iregress $INCLUDES regress/main.c regress/target.cpp $LIBS -o "$b/target"
 "$b/target"
 "$b/baseline" ;;
*) exit 2;;
esac
