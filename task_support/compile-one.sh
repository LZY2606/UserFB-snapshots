#!/bin/bash
set -euo pipefail
src=$1
obj=.task-build/${src%.*}.o
mkdir -p "$(dirname "$obj")"
opts=(-O1 -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t -DBSD=1 -I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include)
if [[ $(uname -s) == Darwin ]]; then opts+=(-DHAVE_SOCKADDR_LEN=1); fi
if [[ $src == *.c ]]; then
  "${CC:-cc}" "${opts[@]}" -c "$src" -o "$obj"
else
  "${CXX:-c++}" -std=c++20 "${opts[@]}" -c "$src" -o "$obj"
fi
