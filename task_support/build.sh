#!/bin/bash
set -euo pipefail
mkdir -p .task-build/include/librist .task-build/obj
cat > .task-build/config.h <<'CONFIG'
#define HAVE_CLOCK_GETTIME 1
#define HAVE_PTHREADS 1
#define HAVE_SOCK_UN_H 1
#define HAVE_LIBMICROHTTPD 0
#define HAVE_PROMETHEUS_SUPPORT 1
#define HAVE_MBEDTLS 0
#define HAVE_NETTLE 0
#define ALLOW_INSECURE_IV_FALLBACK 0
CONFIG
sed 's/@HAVE_SRP_SUPPORT@/0/g' include/librist/config.h.in > .task-build/include/librist/librist_config.h
sed -e 's/@LIBRIST_API_VERSION_MAJOR@/4/g' -e 's/@LIBRIST_API_VERSION_MINOR@/12/g' -e 's/@LIBRIST_API_VERSION_PATCH@/0/g' -e 's/@LIBRIST_API_VERSION@/0x41200/g' include/librist/version.h.in > .task-build/include/librist/version.h
sed 's/@VCS_TAG@/e1da2519fae898daad9606db0914e9bbd0b5bd31/g' include/vcs_version.h.in > .task-build/vcs_version.h
case $(uname -s) in
 Darwin) platform=src/tun_darwin.c ;;
 Linux) platform=src/tun_linux.c ;;
 *) echo 'Linux or Darwin required' >&2; exit 2 ;;
esac
find src -name '*.c' ! -name 'tun_*.c' ! -name eap.c ! -name 'srp*.c' ! -name eap_v4_crypto.c | LC_ALL=C sort > .task-build/sources
printf '%s\n' "$platform" contrib/aes.c contrib/sha256.c contrib/fastpbkdf2.c contrib/stdio-shim.c contrib/time-shim.c contrib/pthread-shim.c contrib/lz4/lz4.c contrib/contrib_cJSON/cjson/cJSON.c >> .task-build/sources
objects=()
while IFS= read -r source; do
 object=".task-build/obj/${source//\//_}.o"
 cc -std=gnu11 -O1 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -I.task-build -I.task-build/include -I.task-build/include/librist -I. -Isrc -Iinclude -Iinclude/librist -Icontrib -Icontrib/lz4 -Icontrib/contrib_cJSON -c "$source" -o "$object"
 objects+=("$object")
done < .task-build/sources
rm -f .task-build/libtaskrist.a
ar rcs .task-build/libtaskrist.a "${objects[@]}"
