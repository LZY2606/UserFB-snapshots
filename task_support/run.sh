#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
b=$(mktemp -d "${TMPDIR:-/tmp}/mpa-cache.XXXXXX")
trap 'rm -rf "$b"' EXIT HUP INT TERM
mkdir -p "$b/mod" "$b/cache" "$b/home"
export GOMODCACHE="$b/mod" GOCACHE="$b/cache" GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
cd "$root"
case ${1:-baseline} in
 baseline) go test -run '^TestCRCBaseline$' -count=1 -v ./task_support ;;
 target)
 test -s crc_contract_test.go
 if go test -run '^TestCRC(ProtectedRegions|Recovery|Reader)$' -count=1 -v . > "$b/out" 2>&1; then cat "$b/out"; else rc=$?;cat "$b/out";exit "$rc";fi
 for name in ProtectedRegions Recovery Reader; do grep -q "^--- PASS: TestCRC${name} " "$b/out" || exit 3;done
 ;;
 *) exit 2;;
esac
