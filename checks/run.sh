#!/bin/bash
set -euo pipefail
if [[ "${1:-target}" == baseline ]]; then node checks/baseline.cjs; else
 b=$(mktemp -d "${TMPDIR:-/tmp}/typr-check.XXXXXX"); trap 'rm -rf "$b"' EXIT
 node --test --test-reporter=tap checks/target.test.cjs | tee "$b/tap"
 node checks/verify-tap.cjs "$b/tap"
 node checks/baseline.cjs
fi
