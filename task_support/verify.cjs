const fs=require('node:fs');
const path=require('node:path');
const assert=require('node:assert/strict');
const log=fs.readFileSync(process.argv[2],'utf8');
const config=require('./config.json');
for(const group of config.groups) {
  assert.ok(log.split('\n').some(l=>l.trim()===`# TASK_GROUP_DONE ${group} checks=1` ||
    new RegExp('^# TASK_GROUP_DONE '+group+' checks=[1-9][0-9]*$').test(l)),`missing completed group ${group}`);
}
assert.ok(!/# (?:SKIP|TODO)\b/.test(log),'skipped tests are not accepted');
const coverage=[];
for(const f of fs.readdirSync(process.argv[3])) {
  if(f.endsWith('.json')) coverage.push(...JSON.parse(fs.readFileSync(path.join(process.argv[3],f))).result);
}
for(const rel of config.production) {
  const suffix='/.task-build/'+rel;
  assert.ok(coverage.some(file=>file.url.endsWith(suffix) && file.functions.some(fn=>
    fn.functionName && fn.ranges.some(r=>r.count>0))),`no executed production function in ${rel}`);
}
console.log('TASK_TARGET_OK');
