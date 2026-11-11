import {test} from 'node:test';
import assert from 'node:assert/strict';
import {sampleFields} from '../sampler.mjs';

test('same getter function on two different objects runs once per object',()=>{
 let reads=0;
 function sharedGetter(){return ++reads;}
 const a={},b={};
 Object.defineProperty(a,'price',{get:sharedGetter});
 Object.defineProperty(b,'price',{get:sharedGetter});
 const root={a,b};
 const out=sampleFields(root,[['a','price'],['b','price'],['a','price'],['b','price']]);
 assert.deepEqual(out.map(r=>r.value),[1,2,1,2]);
 assert.equal(reads,2);
 assert.deepEqual(sampleFields(root,[['a','price'],['b','price']]).map(r=>r.value),[3,4]);
});

test('missing observation is cached even when a later getter creates the property',()=>{
 const row={};
 Object.defineProperty(row,'fill',{get(){row.late=42;return true;}});
 const out=sampleFields(row,[['late'],['fill'],['late']]);
 assert.deepEqual(out[0],{status:'missing',at:0});
 assert.deepEqual(out[1],{status:'value',value:true});
 assert.deepEqual(out[2],{status:'missing',at:0});
 assert.equal(row.late,42);
 assert.deepEqual(sampleFields(row,[['late']]),[{status:'value',value:42}]);
});

test('cached exception reused along aliases reports at per path',()=>{
 const failure=new Error('boom');
 let reads=0;
 const shared={get bad(){reads++;throw failure;}};
 const root={a:shared,wrap:{a:shared}};
 const out=sampleFields(root,[['a','bad'],['wrap','a','bad'],['wrap','a','bad','deeper']]);
 assert.equal(reads,1);
 assert.equal(out[0].status,'error');assert.equal(out[0].at,1);assert.equal(out[0].error,failure);
 assert.equal(out[1].status,'error');assert.equal(out[1].at,2);assert.equal(out[1].error,failure);
 assert.equal(out[2].status,'error');assert.equal(out[2].at,2);assert.equal(out[2].error,failure);
});
