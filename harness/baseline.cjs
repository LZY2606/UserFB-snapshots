const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const jsonata=require('../src/jsonata');
(async()=>{
const text='left@$l.right[$l.id = id].{"l":$l.name,"r":name}';
const e=jsonata(text);let comparisons=0;e.assign(Symbol.for('jsonata.__evaluate_entry'),n=>{if(n.type==='binary'&&n.value==='=')comparisons++;});
const data={left:[{id:1,name:'A'},{id:2,name:'B'}],right:[{id:1,name:'x'},{id:1,name:'y'},{id:3,name:'z'}]};
assert.deepEqual(JSON.parse(JSON.stringify(await e.evaluate(data))),[{l:'A',r:'x'},{l:'A',r:'y'}]);
assert.equal(comparisons,6);console.log('CURRENT_JOIN_COMPARISONS='+comparisons);
const base='test/test-suite',group=path.join(base,'groups/joins');let cases=0;
for(const c of JSON.parse(fs.readFileSync(path.join(group,'library-joins.json')))){
 if(!Object.hasOwn(c,'result'))continue;
 const expr=c.expr||fs.readFileSync(path.join(group,c['expr-file']),'utf8');
 const input=JSON.parse(fs.readFileSync(path.join(base,'datasets',c.dataset+'.json')));
 assert.deepEqual(JSON.parse(JSON.stringify(await jsonata(expr).evaluate(input,c.bindings))),c.result);cases++;
}
assert.ok(cases>=10);console.log('OLD_JOIN_CASES='+cases);console.log('JSONATA_BASELINE_OK');
})().catch(e=>{console.error(e);process.exitCode=1;});
