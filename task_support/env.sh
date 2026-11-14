set -euo pipefail
cd "$(dirname "$0")/.."
export GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off CGO_ENABLED=0
export GOFLAGS=-mod=vendor
cache=$(mktemp -d "${TMPDIR:-/tmp}/go.XXXXXX")
trap 'rm -rf "$cache"' EXIT
export GOCACHE="$cache/build" GOMODCACHE="$cache/mod" GOPATH="$cache/path"
# A module with no external dependencies also works with -mod=vendor.
