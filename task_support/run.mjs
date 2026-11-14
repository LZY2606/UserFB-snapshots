import { spawnSync } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { statSync } from 'node:fs';
const target='test/scene_raycast_index.test.mjs';
if(!statSync(target).isFile() || !statSync(target).size) throw Error('missing or empty target');
for(const group of ['lifecycle','geometry','query','work']) {
 const token=randomUUID();
 const r=spawnSync(process.execPath,['task_support/driver.mjs',target,group,token],{encoding:'utf8',timeout:60000});
 process.stdout.write(r.stdout??'');process.stderr.write(r.stderr??'');
 if(r.error || r.status!==0 || !(r.stdout??'').split('\n').some(s=>s.startsWith(`GROUP_COMPLETE ${group} checks=`)&&s.endsWith(` token=${token}`)&&/checks=[1-9][0-9]* /.test(s))) throw Error(`group incomplete: ${group}`);
}
console.log('TARGET_COMPLETE groups=4');
