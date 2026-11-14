set -euo pipefail
export GOENV=off GOTOOLCHAIN=local GOWORK=off GOPROXY=off GOSUMDB=off GOTELEMETRY=off CGO_ENABLED=0 GOFLAGS=-mod=vendor
task_cache=$(mktemp -d "${TMPDIR:-/tmp}/rtcp-task.XXXXXX")
export GOCACHE="$task_cache/build" GOMODCACHE="$task_cache/mod" GOPATH="$task_cache/path"
task_cleanup() { rm -rf "$task_cache"; }
trap task_cleanup EXIT
