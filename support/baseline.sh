#!/usr/bin/env bash
set -euo pipefail
cache=$(mktemp -d)
trap 'chmod -R u+w "$cache"; rm -rf "$cache"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOCACHE="$cache/build" GOMODCACHE="$cache/modules"
go test -mod=readonly -count=1 -v -run '^(TestDecode|TestMatchHostBasic|TestMatchMixedGetAll|TestMatchStringRoundTrip)$' . > "$cache/old.log"
cat "$cache/old.log"
for name in TestDecode TestMatchHostBasic TestMatchMixedGetAll TestMatchStringRoundTrip; do grep -q -- "--- PASS: $name " "$cache/old.log"; done
echo SSH_CONFIG_BASELINE_MATCH_OK
