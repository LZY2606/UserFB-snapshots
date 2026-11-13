#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
./genMakefiles macosx-no-openssl
for dir in UsageEnvironment BasicUsageEnvironment groupsock liveMedia; do
 make -C "$dir" -j4
 done
