import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {readFileSync,statSync} from 'node:fs';
import {randomBytes} from 'node:crypto';
import {fileURLToPath} from 'node:url';
process.chdir(fileURLToPath(new URL('../',import.meta.url)));
const config=JSON.parse(readFileSync(new URL('./config.json',import.meta.url)));
const baseline=spawnSync(process.execPath,[config.baseline],{encoding:'utf8',timeout:30000});
process.stdout.write(baseline.stdout??'');process.stderr.write(baseline.stderr??'');
assert.equal(baseline.status,0,'baseline failed');
assert.ok(statSync(config.target).isFile(),'target missing');
assert.ok(readFileSync(config.target,'utf8').trim().length>0,'empty target');
for(const group of config.groups){
 const token=randomBytes(24).toString('hex');
 const r=spawnSync(process.execPath,['task_support/driver.mjs',config.target,group,token],{encoding:'utf8',timeout:30000});
 process.stdout.write(r.stdout??'');process.stderr.write(r.stderr??'');
 assert.equal(r.status,0,`target group ${group} failed`);
 const lines=(r.stdout??'').split(/\r?\n/);
 assert.ok(lines.some(x=>x.startsWith(`TASK_GROUP_COMPLETE ${token} ${group} checks=`)),`group ${group} did not finish`);
}
console.log('TARGET_ALL_GROUPS_OK');
