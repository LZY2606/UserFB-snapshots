'use strict';
const {createContext}=require('./check.cjs');
const groups=['candidates','cuts','periodic','replay'];
const tests=require('../test/quilt-contract.js');
if(!tests||typeof tests!=='object')throw new Error('test exports must be a group object');
let total=0;
for(const group of groups){
 if(typeof tests[group]!=='function')throw new Error(`missing group ${group}`);
 const state=createContext(group);
 const ret=tests[group](state.c);
 if(ret&&typeof ret.then==='function')throw new Error('groups must complete synchronously');
 const checks=state.finish();total+=checks;
 console.log(`GROUP ${group} cases=1 checks=${checks} COMPLETE`);
}
console.log(`TARGET_COMPLETE cases=${groups.length} checks=${total}`);
