#!/bin/bash
set -euo pipefail
export GOPROXY=off GOSUMDB=off
b=$(mktemp -d)
trap 'chmod -R u+w "$b"; rm -rf "$b"' EXIT
GOCACHE="$b/build" GOMODCACHE="$b/modules" go run -mod=readonly ./support/baseline
