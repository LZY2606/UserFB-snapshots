#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

export GOPROXY=off
export GOSUMDB=off
export GOTOOLCHAIN=local
export GOCACHE="${TMPDIR:-/tmp}/go-cache"

go test -count=1 -run '^TestEnhancedVideoHeaders$' -v ./format/flv
go test -count=1 -run '^TestEnhancedVideoTracks$' -v ./format/flv
go test -count=1 -run '^TestEnhancedVideoStream$' -v ./format/flv

go run ./task_support/checktarget

# Existing AVC/AAC/script/chunk behavior.
sh ./task_support/baseline.sh
