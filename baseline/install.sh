#!/bin/sh
set -eu
. ./baseline/go-env.sh
go version | grep -F 'go1.26.4 '
go mod download all
go mod verify
go list -m all
