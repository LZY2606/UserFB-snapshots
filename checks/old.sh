set -euo pipefail
export GOPROXY=off GOSUMDB=off
go test -mod=vendor ./auparse ./aucoalesce -run '^(TestFieldMap|TestExtractAndEnrichData|TestCoalesceMessages)$' -v -count=1
echo AUDIT_OLD_TESTS_OK
