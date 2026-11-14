#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
bash task_support/install.sh
if [[ $mode == target ]]; then
  test -s referral_target_test.go
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  bash task_support/go.sh test -vet=off -mod=vendor -count=1 -timeout=45s -v -run '^(TestReferralAlternatives|TestReferralQueryGraph|TestReferralDelivery)$' . >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in TestReferralAlternatives TestReferralQueryGraph TestReferralDelivery; do
    grep -Eq "^=== RUN +${group}$" "$output_log"
    grep -Eq "^--- PASS: ${group} " "$output_log"
    if grep -Eq "^[[:space:]]*--- (SKIP|FAIL): ${group}([/ ]|$)" "$output_log"; then exit 4; fi
    grep -Eq "TASK_GROUP_DONE ${group} checks=[1-9][0-9]*$" "$output_log"
  done
  grep -qx 'PASS' "$output_log"
fi
bash task_support/go.sh run -mod=vendor ./task_support/witness
bash task_support/go.sh test -vet=off -mod=vendor -count=1 -timeout=30s -v -run '^(TestFilter|TestFilterEncode|TestFilterValueUnescape|TestFilterValueEscape)$' .
