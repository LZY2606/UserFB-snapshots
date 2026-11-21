#!/bin/bash
set -euo pipefail
out=$1
shift
if [[ $(uname -s) == Linux ]]; then set -- -DUVGRTP_HAVE_SENDMMSG=1 "$@"; fi
c++ -std=c++17 -pthread -DUVGRTP_HAVE_SENDMSG=1 -Iinclude -Isrc -I.task-build/include -I. "$@" .task-build/libuvgrtp.a -o "$out"
