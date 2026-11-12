#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'chmod -R u+w "$build"; rm -rf "$build"' EXIT
export GOCACHE="$build/cache" GOMODCACHE="$build/mod" GOPROXY=off GOSUMDB=off
export GOTOOLCHAIN=local GOENV=off GOWORK=off
export GOFLAGS="-modfile=devcheck/offline.mod -mod=readonly"
case "${1:?install, baseline or target}" in
 install) go build . ./devcheck/baseline ./devcheck/check; echo QFRAME_OFFLINE_INSTALL_OK;;
 baseline) go run ./devcheck/baseline; go test -count=1 -v . -run '^TestQFrame_(SortStability|GroupByAggregate|AggregateGroupByNull|RollingWindow)$';;
 target) go run ./devcheck/check;;
 *) exit 2;;
esac
