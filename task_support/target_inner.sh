source "$(dirname "$0")/env.sh"
test -f format/mp4/sample_clock_test.go
log="$cache/target.log"
set +e
go test -count=1 -v ./format/mp4 -run '^(TestClockTicks|TestClockLongHeaders|TestClockReadback)$' >"$log" 2>&1
status=$?
set -e
cat "$log"
test "$status" -eq 0
! grep -Eq -- '--- SKIP:|no tests to run|\[no test files\]' "$log"
grep -Eq -- '^=== RUN   TestClockTicks$' "$log"
grep -Eq -- '^--- PASS: TestClockTicks \(.*\)$' "$log"
grep -Eq -- '^=== RUN   TestClockLongHeaders$' "$log"
grep -Eq -- '^--- PASS: TestClockLongHeaders \(.*\)$' "$log"
grep -Eq -- '^=== RUN   TestClockReadback$' "$log"
grep -Eq -- '^--- PASS: TestClockReadback \(.*\)$' "$log"
grep -qx 'PASS' "$log"
go test -count=1 -v ./task_support
