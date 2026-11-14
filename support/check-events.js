'use strict';
const fs=require('node:fs');
const groups=['TestMemoryBranches','TestMemoryLoops','TestMemoryCalls','TestMemoryArithmetic','TestMemoryIsolation'];
const events=fs.readFileSync(process.argv[2],'utf8').trim().split('\n').filter(Boolean).map(x=>JSON.parse(x));
if(events.some(e=>e.Action==='fail'||e.Action==='skip'))throw new Error('failed or skipped execution');
for(const name of groups){
 if(!events.some(e=>e.Action==='run'&&e.Test===name)||!events.some(e=>e.Action==='pass'&&e.Test===name))throw new Error('missing PASS '+name);
 if(!events.some(e=>e.Action==='pass'&&e.Test&&e.Test.startsWith(name+'/')))throw new Error('zero passed subtests '+name);
}
console.log('TARGET_COMPLETE');
