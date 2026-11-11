#!/usr/bin/env bash
set -euo pipefail
test -s src/preprocessor/rescan.test.ts
report=$(mktemp); trap 'rm -f "$report"' EXIT
./node_modules/.bin/vitest run src/preprocessor/preprocessor.test.ts src/preprocessor/rescan.test.ts --reporter=json --outputFile="$report"
node --input-type=module - "$report" <<'JS'
import fs from 'node:fs'; import assert from 'node:assert/strict';
const r=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
assert.ok(r.numTotalTests>39); assert.equal(r.numFailedTests,0);
const assertions=r.testResults.filter(x=>x.name.endsWith('/rescan.test.ts')).flatMap(x=>x.assertionResults);
for(const name of ['alias-after-expansion','recursive-token-suppression','conditional-rescan','shader-ast-roundtrip']) assert.ok(assertions.some(x=>x.title===name && x.status==='passed'),name);
console.log('GLSL_RESCAN_OK');
JS
