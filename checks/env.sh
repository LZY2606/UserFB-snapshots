cache="${TMPDIR:-/tmp}/canvas-7d75044-cache"
mkdir -p "$cache/mod" "$cache/build" "$cache/gopath"
export GOMODCACHE="$cache/mod" GOCACHE="$cache/build" GOPATH="$cache/gopath" GOTOOLCHAIN=local
