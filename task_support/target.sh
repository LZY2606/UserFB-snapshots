#!/bin/sh
set -eu
export GOPROXY=off GOSUMDB=off GOTOOLCHAIN=local
export GOCACHE="${TMPDIR:-/tmp}/go-cache"

REC="${TMPDIR:-/tmp}/rtmp-enhanced-taskcheck.json"
rm -f "$REC"
export TASKCHECK_FILE="$REC"

# Record the enhanced video cases and assertions through taskcheck.
go test -count=1 -run 'TestEnhancedVideo' ./format/flv/...

# Verify the completion records and the minimum assertion counts.
go run ./task_support/check

# Original related tests and the legacy AVC baseline must keep passing.
go run ./task_support/baseline.go
go test -count=1 ./format/flv/... ./format/rtmp/... ./utils/bits/...
