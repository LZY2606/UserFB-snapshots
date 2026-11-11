#!/bin/bash
set -euo pipefail
for file in core/lane_cookie_lifecycle_test.go cmd/lane_cookie_lifecycle_test.go; do
 if [ ! -s "$file" ]; then echo "TARGET_MISSING_FILE: $file" >&2; exit 2; fi
done
mkdir -p .build
pattern='^TestLaneCookieLifecycle(Store|Browser|Challenge)$'
bash baseline/go-env.sh go test ./core ./cmd -list "$pattern" > .build/lane-target-list.log
for name in TestLaneCookieLifecycleStore TestLaneCookieLifecycleBrowser TestLaneCookieLifecycleChallenge; do
 if ! grep -qx "$name" .build/lane-target-list.log; then echo "TARGET_MISSING_TEST: $name" >&2; exit 3; fi
done
set +e
bash baseline/go-env.sh go test -race ./core ./cmd -run "$pattern" -count=1 -timeout=60s -v > .build/lane-target.log 2>&1
status=$?
set -e
cat .build/lane-target.log
if [ "$status" -ne 0 ]; then exit "$status"; fi
if grep -q -- '--- SKIP:' .build/lane-target.log; then echo TARGET_SKIPPED_TEST >&2; exit 4; fi
for name in TestLaneCookieLifecycleStore TestLaneCookieLifecycleBrowser TestLaneCookieLifecycleChallenge; do
 grep -q "^--- PASS: $name " .build/lane-target.log
done
bash baseline/old-tests.sh
