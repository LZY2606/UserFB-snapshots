#!/bin/sh
set -eu
mode=${1:-baseline}
case "$mode" in
 baseline) src=task_support/baseline.cc ;;
 target) src=task_support/target.cc; test -f "$src" ;;
 *) exit 2 ;;
esac
ssl_prefix=$(pkg-config --variable=prefix openssl)
${CXX:-c++} -std=c++11 -pthread -I_include -I"$ssl_prefix/include" "$src" _lib/libworkflow.a -L"$ssl_prefix/lib" -lssl -lcrypto -o ".task-build/$mode"
".task-build/$mode"
