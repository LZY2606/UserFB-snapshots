import fs from 'node:fs';
import assert from 'node:assert/strict';
const s=fs.readFileSync(process.argv[2],'utf8');
for(const name of ['QuarantineBoundary','ProbePurity','ChunkReentry','LegacyStrict'])
 assert(new RegExp('^ok [0-9]+ - '+name+'$','m').test(s),'target group did not execute: '+name);
assert(/^# tests [1-9][0-9]*$/m.test(s),'zero tests');
assert(/^# skipped 0$/m.test(s),'skipped tests');
assert(/^# fail 0$/m.test(s),'failed tests');
console.log('CSV_TARGET_GROUPS_EXECUTED');
