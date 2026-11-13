#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
sh task_support/install.sh
mode=${1:-baseline}
if [ "$mode" = baseline ]; then set -- task_support/witness.cpp
elif [ "$mode" = target ]; then
 test -s test/rtx-recovery.cpp
 make -C test CXXFLAGS='-std=c++20 -O1 -g -Wall -Wextra' test_interleaved_framing
 ./test/test_interleaved_framing
 set -- task_support/target_main.cpp test/rtx-recovery.cpp
else exit 2; fi
${CXX:-c++} -std=c++20 -O1 -g -DNO_OPENSSL=1 -DBSD=1 -DSOCKLEN_T=socklen_t -DHAVE_SOCKADDR_LEN=1 -I liveMedia/include -I groupsock/include -I BasicUsageEnvironment/include -I UsageEnvironment/include "$@" liveMedia/libliveMedia.a groupsock/libgroupsock.a BasicUsageEnvironment/libBasicUsageEnvironment.a UsageEnvironment/libUsageEnvironment.a -o ".task-build/$mode"
exec ".task-build/$mode"
