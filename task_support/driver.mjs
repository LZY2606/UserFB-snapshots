import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
import {resolve} from 'node:path';
const [target,group,token]=process.argv.slice(2);
const loaded=await import(pathToFileURL(resolve(target)).href);
const cases=loaded.cases??loaded.default?.cases;
assert.ok(cases && typeof cases==='object' && !Array.isArray(cases),'cases object required');
const run=cases[group];
assert.equal(typeof run,'function',`missing group ${group}`);
assert.ok(!run.skip,`skipped group ${group}`);
let checks=0,failed=false;
const check=(value,message='content assertion')=>{
 checks++; if(!value) failed=true;
 assert.ok(value,message);
};
await run(check);
assert.ok(checks>0 && !failed,'successful content assertions required; caught failures stay failed');
console.log(`TASK_GROUP_COMPLETE ${token} ${group} checks=${checks}`);
