#!/bin/sh
set -eu
export GOPROXY=off GOSUMDB=off GOFLAGS=-mod=vendor
mkdir -p .task-cache
export GOCACHE="$PWD/.task-cache"
case "${1:-baseline}" in
 baseline) go run ./task_support/baseline ;;
 target) test -f session/autoattach_target_test.go; go test -race ./session ./rpcc -count=1 -timeout=45s ;;
 *) exit 2 ;;
esac
