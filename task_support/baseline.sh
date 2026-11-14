#!/bin/bash
set -euo pipefail
bash task_support/go.sh run -mod=vendor ./task_support/witness
bash task_support/go.sh test -mod=vendor -count=1 -run '^(TestCollectorVisit|TestCollectorPostRaw|TestSetCookieRedirect)$' .
