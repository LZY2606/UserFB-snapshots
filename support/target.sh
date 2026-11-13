#!/bin/bash
set -euo pipefail
export GOPROXY=off GOSUMDB=off
b=$(mktemp -d)
trap 'chmod -R u+w "$b"; rm -rf "$b"' EXIT
export GOCACHE="$b/build" GOMODCACHE="$b/modules"
go run -mod=readonly ./support/check-file
go test -mod=readonly -count=1 -json -v ./analysis -run '^TestMemory(Branches|Loops|Calls|Arithmetic|Isolation)$' | tee "$b/events"
node support/check-events.js "$b/events"
bash support/baseline.sh
