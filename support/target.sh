#!/usr/bin/env bash
set -euo pipefail
cache=$(mktemp -d)
trap 'chmod -R u+w "$cache"; rm -rf "$cache"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local GOCACHE="$cache/build" GOMODCACHE="$cache/modules"
go run -mod=readonly ./support/testguard '^TestResolve(Phases|Include|Syntax)$' TestResolvePhases TestResolveInclude TestResolveSyntax
bash support/baseline.sh
