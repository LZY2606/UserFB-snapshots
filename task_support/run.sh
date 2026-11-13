#!/bin/sh
set -eu
mkdir -p .task-build
incs="-IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include"
libs="liveMedia/libliveMedia.a groupsock/libgroupsock.a BasicUsageEnvironment/libBasicUsageEnvironment.a UsageEnvironment/libUsageEnvironment.a"
case "${1:-baseline}" in
 baseline)
 c++ -std=c++20 -DNO_OPENSSL=1 $incs task_support/baseline.cpp $libs -o .task-build/baseline
 .task-build/baseline ;;
 target)
 sh task_support/install.sh
 c++ -std=c++20 -DNO_OPENSSL=1 $incs task_support/target_main.cpp test/aac-au-timing.cpp $libs -o .task-build/target
 .task-build/target
 c++ -std=c++20 -IliveMedia/include test/test_interleaved_framing.cpp -o .task-build/interleaved
 .task-build/interleaved ;;
 *) exit 2 ;;
esac
