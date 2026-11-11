import assert from 'node:assert/strict';
import {existsSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
const file='test/functional/modules/module-archive.test.js';
assert.ok(existsSync(file),`Add ${file}`);
const r=spawnSync(process.execPath,['node_modules/mocha/bin/mocha.js','--timeout','7000','--reporter','json',file],{encoding:'utf8'});
process.stdout.write(r.stdout); process.stderr.write(r.stderr);
assert.equal(r.status,0,'module archive tests failed');
const report=JSON.parse(r.stdout);
assert.ok(report.stats.tests>=6); assert.equal(report.stats.pending,0);
assert.equal(report.stats.passes,report.stats.tests);
for(const title of ['http graph','document maps','redirect scopes','cycles','unresolved imports','legacy mode'])
 assert.ok(report.passes.some(t=>t.fullTitle.includes(title)),`Missing passed case ${title}`);
console.log('MODULE_ARCHIVE_TARGET_OK');
