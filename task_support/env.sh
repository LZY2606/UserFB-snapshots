set -euo pipefail
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off CGO_ENABLED=0 GOWORK=off
export GOFLAGS=-mod=vendor
cache=$(mktemp -d "${TMPDIR:-/tmp}/check-go.XXXXXX")
trap 'rm -rf "$cache"' EXIT
export GOCACHE="$cache/build" GOMODCACHE="$cache/mod" GOPATH="$cache/path"
