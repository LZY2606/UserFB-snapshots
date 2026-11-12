#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOENV=off GOTOOLCHAIN=local
export GOPATH="$PWD/.cache/gopath" GOCACHE="$PWD/.cache/gobuild"
if [[ "$(go env GOVERSION)" != go1.26.4 ]]; then
  echo 'This snapshot uses Go 1.26.4; select that toolchain first.' >&2
  exit 2
fi
exec go "$@"
