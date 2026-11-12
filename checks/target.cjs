const {spawnSync}=require('node:child_process');
const assert=require('node:assert/strict');
const names=["TestExecveAssemblyRecords", "TestExecveAssemblyFragments", "TestExecveAssemblyEvidence", "TestExecveAssemblyIdentity"];
const r=spawnSync('go',['test','-mod=vendor','-count=1','-json','-run','^('+names.join('|')+')$','./aucoalesce'],{encoding:'utf8',env:{...process.env,GOPROXY:'off',GOSUMDB:'off'}});
process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');
assert.equal(r.status,0,'go test failed');
const events=r.stdout.split('\n').filter(x=>x.startsWith('{')).map(x=>JSON.parse(x));
for(const name of names){
 assert(events.some(e=>e.Action==='run'&&e.Test===name),'missing run '+name);
 assert(events.some(e=>e.Action==='pass'&&e.Test===name),'missing PASS '+name);
 assert(!events.some(e=>e.Action==='skip'&&(e.Test===name||e.Test?.startsWith(name+'/'))),'skipped '+name);
}
console.log('TARGET_GROUPS_OK '+names.length);
