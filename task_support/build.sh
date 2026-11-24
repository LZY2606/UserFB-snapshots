#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
# Equivalent single-thread POSIX configuration; do not run obsolete autoconf.
cat > src/rtptypes_unix.h <<'TYPES'
#ifndef RTPTYPES_H
#define RTPTYPES_H
#include <sys/types.h>
#include <stdint.h>
#endif
TYPES
cat > src/rtpconfig_unix.h <<'CONFIG'
#ifndef RTPCONFIG_UNIX_H
#define RTPCONFIG_UNIX_H
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define RTP_BIG_ENDIAN
#endif
#define RTP_SOCKLENTYPE_UINT
#if defined(__APPLE__) || defined(__FreeBSD__)
#define RTP_HAVE_SOCKADDR_LEN
#endif
#define RTP_SUPPORT_IPV4MULTICAST
#define RTP_SUPPORT_SDESPRIV
#define RTP_SUPPORT_INLINETEMPLATEPARAM
#define RTP_SUPPORT_PROBATION
#endif
CONFIG
objects=()
for f in src/*.cpp; do
 obj=".task-build/$(basename "${f%.cpp}").o"
 c++ -std=c++11 -pthread -Isrc -I. -c "$f" -o "$obj"
 objects+=("$obj")
done
rm -f .task-build/libjrtp.a
ar rcs .task-build/libjrtp.a "${objects[@]}"
