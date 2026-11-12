#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh checks/go-env.sh go test ./pt -run '^TestEnvironmentSeed(Sky|Diffuse|Mirror)$' -count=1 -v
mkdir -p .cache
sh checks/go-env.sh go build -o .cache/hello examples/hello.go
printf 'LEGACY_HELLO_BUILD_OK\n'
