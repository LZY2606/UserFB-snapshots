'use strict';
const fs=require('node:fs'),assert=require('node:assert/strict'),cp=require('node:child_process');
const {withBundle}=require('./bundle.cjs');
assert.ok(fs.existsSync('tests/composition-contract.ts'),'add tests/composition-contract.ts');
withBundle('tests/composition-contract.ts',async built=>{
 const p=cp.spawnSync(process.execPath,['--test','--test-reporter=tap',built],{encoding:'utf8',timeout:60000});
 const s=(p.stdout||'')+(p.stderr||'');process.stdout.write(s);
 assert.equal(p.status,0,'composition contract must pass');
 assert.match(s,/^ok \d+ - Composition editing contract$/m);
 assert.match(s,/^# pass [1-9][0-9]*$/m);
 for(const k of ['fail','cancelled','skipped'])assert.match(s,new RegExp('^# '+k+' 0$','m'));
 for(const m of ['IME_RANGE_OK','IME_EVENTS_OK','IME_SESSION_OK'])assert.ok(s.includes(m),m);
 const old=cp.spawnSync(process.execPath,['support/baseline.cjs'],{encoding:'utf8'});
 process.stdout.write((old.stdout||'')+(old.stderr||''));assert.equal(old.status,0);
}).catch(e=>{console.error(e);process.exitCode=1});
