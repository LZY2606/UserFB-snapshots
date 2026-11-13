#!/bin/sh
set -eu
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOCACHE="${TMPDIR:-/tmp}/go-cache"
go run ./task_support/baseline.go
go test -count=1 ./format/flv/... ./format/rtmp/... ./utils/bits/...
