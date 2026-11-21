import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {readFileSync} from 'node:fs';
const file='test/style-update.test.mjs';
assert.ok(readFileSync(file,'utf8').trim(),'target test must not be empty');
const result=spawnSync(process.execPath,['--test','--test-reporter=tap',file],{encoding:'utf8',timeout:180000,maxBuffer:16*1024*1024});
process.stdout.write(result.stdout??'');
process.stderr.write(result.stderr??'');
assert.ifError(result.error);
assert.equal(result.signal,null,'test terminated by signal');
assert.equal(result.status,0,'test command failed');
const out=result.stdout;
assert.ok(!/\bSKIP\b|\bTODO\b/.test(out),'skipped or todo tests are not accepted');
for(const name of ['defaults','origins','live-edits','persistence','compatibility']){
 const re=new RegExp(`^# TASK_GROUP_DONE ${name} checks=[1-9][0-9]*$`,'m');
 assert.ok(re.test(out),`missing executed group: ${name}`);
}
assert.match(out,/^# TASK_SUITE_COMPLETE groups=5$/m);
assert.match(out,/^# tests [1-9][0-9]*$/m);
assert.match(out,/^# fail 0$/m);
assert.match(out,/^# cancelled 0$/m);
assert.match(out,/^# skipped 0$/m);
