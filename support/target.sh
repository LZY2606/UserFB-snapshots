#!/usr/bin/env bash
set -euo pipefail
export GOWORK=off
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
export GOTMPDIR="$b"
test -s records_test.go
go test -count=1 -v -run '^TestRecordStreamContract$' . | tee "$b/target.log"
grep -Fq -- '--- PASS: TestRecordStreamContract ' "$b/target.log"
for m in RECORD_OVERLAP_OK RECORD_CONTEXT_OK RECORD_PRUNE_OK; do grep -Fq "$m" "$b/target.log"; done
go test -count=1 -run '^(TestRecordBaseline|TestStreamParser_Success1|TestStreamParser_Success2|TestStreamNamespaceScopeBetweenRecords|TestOutputXMLWithNamespacePrefix)$' .
