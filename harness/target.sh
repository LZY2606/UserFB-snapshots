#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
GOWORK=off GOPROXY=off GOSUMDB=off GOMODCACHE="$build/gomod" go test -mod=readonly . -run '^(TestCaptureBaseline|TestLogicalTextMarked|TestLogicalTextForms|TestLogicalTextErrors)$' -count=1 -v | tee "$build/target.log"
if grep -Eq '^[[:space:]]*--- (SKIP|FAIL):' "$build/target.log"; then exit 1; fi
for name in TestCaptureBaseline TestLogicalTextMarked TestLogicalTextForms TestLogicalTextErrors; do
 grep -Eq "^--- PASS: $name \(" "$build/target.log"
done
printf 'LOGICAL_TEXT_TARGET_OK\n'
