#!/bin/sh
set -eu

# Build the no-OpenSSL live555 libraries if needed, then build and run the
# local two-packet MPEG4-GENERIC AU timing example.
sh task_support/install.sh

c++ -std=c++20 -DNO_OPENSSL=1 \
  -IliveMedia/include -Igroupsock/include \
  -IUsageEnvironment/include -IBasicUsageEnvironment/include \
  examples/aac_au_example.cpp \
  liveMedia/libliveMedia.a groupsock/libgroupsock.a \
  BasicUsageEnvironment/libBasicUsageEnvironment.a \
  UsageEnvironment/libUsageEnvironment.a \
  -o .task-build/aac_au_example

.task-build/aac_au_example
