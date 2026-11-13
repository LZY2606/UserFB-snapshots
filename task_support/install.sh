#!/bin/sh
set -eu
case "$(uname -s)" in
 Darwin) sh genMakefiles macosx-no-openssl ;;
 Linux) sh genMakefiles linux-no-openssl ;;
 *) echo "requires Darwin or Linux" >&2; exit 2 ;;
esac
for d in UsageEnvironment BasicUsageEnvironment groupsock liveMedia; do
 make -C "$d" -j2
done
