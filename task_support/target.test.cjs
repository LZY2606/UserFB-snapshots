const test=require('node:test');const assert=require('node:assert/strict');
const cases=require('./target-cases.cjs');
for(const group of ['mapping','retiming','repeat']) test(group,{concurrency:false},async()=>{
 let count=0;const check={};for(const method of ['ok','equal','deepEqual','throws','rejects','notEqual'])check[method]=(...args)=>{count++;return assert[method](...args)};
 assert.equal(typeof cases[group],'function','missing group '+group);
 await cases[group](check);assert.ok(count>0,'zero assertions '+group);
 console.log('TASK_GROUP '+group+' checks='+count+' complete');
});
