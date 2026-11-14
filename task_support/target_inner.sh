source "$(dirname "$0")/env.sh"
test -f task_checks/program_clock_test.go
log="$cache/target.log"
set +e
go test -count=1 -v ./task_checks -run '^(TestProgramClockWrap|TestProgramClockSnapshot|TestProgramClockEpoch)$' >"$log" 2>&1
status=$?
set -e
cat "$log"
test "$status" -eq 0
! grep -Eq -- '--- SKIP:|no tests to run|\[no test files\]' "$log"
grep -Eq -- '^=== RUN   TestProgramClockWrap$' "$log"
grep -Eq -- '^--- PASS: TestProgramClockWrap \(.*\)$' "$log"
grep -Eq -- '^=== RUN   TestProgramClockSnapshot$' "$log"
grep -Eq -- '^--- PASS: TestProgramClockSnapshot \(.*\)$' "$log"
grep -Eq -- '^=== RUN   TestProgramClockEpoch$' "$log"
grep -Eq -- '^--- PASS: TestProgramClockEpoch \(.*\)$' "$log"
grep -qx 'PASS' "$log"
go test -count=1 -v ./task_support
go test -count=1 -v . -run '^(TestDemuxerNextDataPATPMT|TestPacketPoolBoundedPESCompletesWithoutNextStart)$'
