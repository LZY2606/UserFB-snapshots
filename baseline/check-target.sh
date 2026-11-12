#!/bin/sh
set -eu
. ./baseline/go-env.sh
export GOPROXY=off
for file in reassembly/overlap_history_test.go examples/tcpoverlap/main.go; do
 test -s "$file" || { echo "missing target source: $file" >&2; exit 1; }
done
mkdir -p .logs
go test -json -race -count=1 ./reassembly -run '^TestOverlapHistory(Queued|Delivered|Lifecycle)$' > .logs/target.jsonl
for name in TestOverlapHistoryQueued TestOverlapHistoryDelivered TestOverlapHistoryLifecycle; do
 grep -Eq '"Action":"pass".*"Test":"'"$name"'"' .logs/target.jsonl || { echo "missing successful test: $name" >&2; exit 1; }
done
if grep -q '"Action":"skip"' .logs/target.jsonl; then echo 'skipped target tests' >&2; exit 1; fi
cat .logs/target.jsonl
go test -count=1 ./reassembly
# The example must check its observations, and exit nonzero on disagreement.
go run ./examples/tcpoverlap > .logs/example.txt
cat .logs/example.txt
grep -qx 'conflicts=2 reassembly=gap!AXCZ' .logs/example.txt
