import {test} from 'node:test';
import assert from 'node:assert/strict';
import {sampleFields} from '../sampler.mjs';
test('aliased runtime field is read once in a sampling call',()=>{
 let reads=0;const shared={get price(){return ++reads;}};
 const root={a:shared,b:shared};const paths=[['a','price'],['b','price'],['a','price']];
 assert.deepEqual(sampleFields(root,paths).map(r=>r.value),[1,1,1]);
 assert.equal(reads,1);
 assert.deepEqual(sampleFields(root,paths).map(r=>r.value),[2,2,2]);
});
test('own undefined, missing and inherited properties differ',()=>{
 let inheritedReads=0;const proto={get inherited(){inheritedReads++;return 8;}};
 const root=Object.assign(Object.create(proto),{present:undefined,empty:null});
 const out=sampleFields(root,[['present'],['absent'],['inherited'],['empty','x']]);
 assert.deepEqual(out,[{status:'value',value:undefined},{status:'missing',at:0},{status:'missing',at:0},{status:'missing',at:1}]);
 assert.equal(inheritedReads,0);
});
test('cached throwing accessor and later independent branch',()=>{
 const failure={reason:'fixture'};let reads=0;const shared={get bad(){reads++;throw failure;},ok:7};
 const out=sampleFields({a:shared,b:shared},[['a','bad','x'],['b','bad'],['a','ok']]);
 assert.equal(reads,1);assert.equal(out[0].error,failure);assert.equal(out[1].error,failure);
 assert.equal(out[0].at,1);assert.equal(out[1].at,1);assert.deepEqual(out[2],{status:'value',value:7});
});
test('first observation survives changes later in the same call',()=>{
 const row={cost:1};Object.defineProperty(row,'change',{get(){row.cost=99;return true;}});
 const out=sampleFields(row,[['cost'],['change'],['cost']]);
 assert.deepEqual(out.map(r=>r.value),[1,true,1]);
 assert.equal(sampleFields(row,[['cost']])[0].value,99);
});
test('no eager reads, coercion, then assimilation or recursive enumeration',()=>{
 let unused=0,thenReads=0;const promiseLike={get then(){thenReads++;throw 0;}};
 const root={wanted:promiseLike,get unused(){unused++;throw 0;}};root.self=root;
 const out=sampleFields(root,[['wanted'],['self','self','wanted']]);
 assert.equal(out[0].value,promiseLike);assert.equal(out[1].value,promiseLike);
 assert.equal(unused,0);assert.equal(thenReads,0);
});
test('all paths validate before any page getter executes',()=>{
 let reads=0;const root={get x(){reads++;return 1;}};
 assert.throws(()=>sampleFields(root,[['x'],['bad',2]]),TypeError);
 assert.throws(()=>sampleFields(root,[[]]),TypeError);
 assert.equal(reads,0);
});
