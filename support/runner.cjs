const assert=require('node:assert/strict');
const cases=[];let checks=0;
const api={add(group,name,fn){assert.equal(typeof fn,'function');cases.push({group,name,fn});},check(value,message){checks++;assert.ok(value,message);}};
const register=require('./target.cjs');assert.equal(typeof register,'function');register(api);
for(const group of ['photometric','masks','compatibility']){
 const selected=cases.filter(c=>c.group===group);assert(selected.length>0,'missing group '+group);
 for(const c of selected){const before=checks;const result=c.fn();assert(!result||typeof result.then!=='function','use synchronous cases');assert(checks>before,'no assertions '+c.name);console.log('PASS '+group+'/'+c.name);}
}
console.log(`TARGET COMPLETE cases=${cases.length} checks=${checks}`);
