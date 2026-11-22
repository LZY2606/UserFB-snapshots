#!/bin/bash
set -euo pipefail
output=$1
shift
inc=(-I. -IliveMedia/include -Igroupsock/include -IBasicUsageEnvironment/include -IUsageEnvironment/include)
if [[ $(uname -s) == Darwin ]]; then
  libs=(-Wl,-all_load .task-build/production.a)
else
  libs=(-Wl,--whole-archive .task-build/production.a -Wl,--no-whole-archive -pthread)
fi
c++ -std=c++20 -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t "${inc[@]}" "$@" "${libs[@]}" -o "$output"
