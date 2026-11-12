#!/bin/sh
set -eu
. ./checks/env.sh
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
go build -mod=readonly -o "$b/baseline" ./checks/baseline
echo CANVAS_DEPENDENCIES_OK
