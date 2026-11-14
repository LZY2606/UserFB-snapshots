source "$(dirname "$0")/env.sh"
test -f mkvcore/timed_reader_test.go
log="$cache/target.log"
set +e
go test -count=1 -v ./mkvcore -run '^(TestTimedLaceClock|TestTimedGroupMetadata|TestTimedReaderFlow)$' >"$log" 2>&1
status=$?
set -e
cat "$log"
test "$status" -eq 0
! grep -Eq -- '--- SKIP:|no tests to run|\[no test files\]' "$log"
grep -Eq -- '^=== RUN   TestTimedLaceClock$' "$log"
grep -Eq -- '^--- PASS: TestTimedLaceClock \(.*\)$' "$log"
grep -Eq -- '^=== RUN   TestTimedGroupMetadata$' "$log"
grep -Eq -- '^--- PASS: TestTimedGroupMetadata \(.*\)$' "$log"
grep -Eq -- '^=== RUN   TestTimedReaderFlow$' "$log"
grep -Eq -- '^--- PASS: TestTimedReaderFlow \(.*\)$' "$log"
grep -qx 'PASS' "$log"
go test -count=1 -v ./task_support
go test -count=1 -v ./mkvcore -run '^(TestBlockReader|TestBlockReader_Close)$'
