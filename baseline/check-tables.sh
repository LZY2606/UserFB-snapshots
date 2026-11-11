#!/bin/bash
set -euo pipefail
mkdir -p .build
node test/run-table-regressions.cjs > .build/table-regressions.log
cat .build/table-regressions.log
for marker in TABLE_BASELINE_OK TABLE_MARK_CAUGHT TABLE_CONTAINER_CAUGHT TABLE_UNWRAP_CAUGHT; do
  grep -qx "$marker" .build/table-regressions.log
done
./node_modules/.bin/mocha test/test-readability.js --grep table --reporter dot
