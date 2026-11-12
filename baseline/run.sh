set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOCACHE=${GOCACHE:-$build/cache}
export GOMODCACHE=${GOMODCACHE:-$build/mod}
go run ./baseline
go test ./ip6defrag -count=1 -v
