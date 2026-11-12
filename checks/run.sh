#!/bin/bash
set -euo pipefail
if [[ "${1:-target}" == baseline ]]; then node --import ./checks/env.mjs checks/baseline.mjs; else
 b=$(mktemp -d "${TMPDIR:-/tmp}/math-check.XXXXXX"); trap 'rm -rf "$b"' EXIT
 node --import ./checks/env.mjs --test --test-reporter=tap checks/target.test.mjs | tee "$b/tap"
 node checks/verify-tap.cjs "$b/tap"
 node --import ./checks/env.mjs checks/baseline.mjs
fi
