set -eu
mkdir -p .build/go-cache .build/go-mod
export GOCACHE="$PWD/.build/go-cache" GOMODCACHE="$PWD/.build/go-mod"
go mod download
