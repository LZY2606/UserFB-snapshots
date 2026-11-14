source "$(dirname "$0")/env.sh"
test -f opendml_test.go
log="$cache/target.log"
set +e
go test -count=1 -v . -run '^(TestOpenDMLSegments|TestOpenDMLIndexes|TestOpenDMLLimit)$' >"$log" 2>&1
status=$?
set -e
cat "$log"
test "$status" -eq 0
! grep -Eq -- '--- SKIP:|no tests to run|\[no test files\]' "$log"
grep -Eq -- '^=== RUN   TestOpenDMLSegments$' "$log"
grep -Eq -- '^--- PASS: TestOpenDMLSegments \(.*\)$' "$log"
grep -Eq -- '^=== RUN   TestOpenDMLIndexes$' "$log"
grep -Eq -- '^--- PASS: TestOpenDMLIndexes \(.*\)$' "$log"
grep -Eq -- '^=== RUN   TestOpenDMLLimit$' "$log"
grep -Eq -- '^--- PASS: TestOpenDMLLimit \(.*\)$' "$log"
grep -qx 'PASS' "$log"
go test -count=1 -v ./task_support
