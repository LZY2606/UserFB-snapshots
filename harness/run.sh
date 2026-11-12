#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
case "${1:-baseline}" in
baseline) node harness/baseline.mjs ;;
target)
 b=$(mktemp -d)
 trap 'rm -rf "$b"' EXIT
 if node --test --test-reporter=tap harness/target.test.mjs > "$b/tap" 2>&1; then
  cat "$b/tap"
 else cat "$b/tap"; exit 1; fi
 node harness/check-tap.mjs "$b/tap"
 node harness/baseline.mjs ;;
*)exit 2;;
esac
