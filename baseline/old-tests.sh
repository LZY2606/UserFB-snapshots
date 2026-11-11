#!/bin/bash
set -euo pipefail
core_cases='TestLaneStoreReusesCookiesBySession|TestLaneStoreDropCookiesPreservesProfile|TestLaneStoreEvictsLRU|TestProxyLaneKeyIncludesTenant|TestCaptchaDropsProxyLaneCookies|TestProxyErrorDoesNotDropProxyLaneCookies|TestBrowserConnectionRejectsCanceledAndClosed'
cmd_cases='TestBrowserPoolKey|TestBrowserPoolEvictLRU|TestBrowserPoolBrowserStats'
bash baseline/go-env.sh go test ./core -run "^(${core_cases})$" -count=1 -v > .build/old-core.log
cat .build/old-core.log
IFS='|' read -ra names <<< "$core_cases"
for name in "${names[@]}"; do grep -q "^--- PASS: $name " .build/old-core.log; done
bash baseline/go-env.sh go test ./cmd -run "^(${cmd_cases})$" -count=1 -v > .build/old-cmd.log
cat .build/old-cmd.log
IFS='|' read -ra names <<< "$cmd_cases"
for name in "${names[@]}"; do grep -q "^--- PASS: $name " .build/old-cmd.log; done
printf '%s\n' BASE_LANE_OLD_TEN_OK
