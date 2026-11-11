#!/usr/bin/env bash
set -euo pipefail
export GOTOOLCHAIN=local
export GOCACHE="$PWD/.build/go-cache"
export GOMODCACHE="$PWD/.build/go-mod"
export GOPATH="$PWD/.build/go-path"
export GOFLAGS=-mod=readonly
test -s redirect_admission_test.go
mkdir -p .build
go test -race -json -count=1 -timeout=40s -run '^TestRedirectAdmission(Policy|HostLease|Cancel|Compatibility)$' . > .build/redirect-admission.jsonl
cat .build/redirect-admission.jsonl
go run ./support/check_target.go .build/redirect-admission.jsonl
