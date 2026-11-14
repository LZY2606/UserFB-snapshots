'use strict';
const assert=require('node:assert/strict');
module.exports=async function run(name,body){
 assert.equal(typeof body,'function',`missing group ${name}`);
 let assertions=0,failed=false;
 const check=Object.freeze({check(ok,message='content check failed'){
  assertions++;if(!ok){failed=true;throw new assert.AssertionError({message});}
 }});
 await body(check);
 assert.ok(!failed,`${name}: assertion was caught`);
 assert.ok(assertions>0,`${name}: no assertions`);
 return {name,assertions,status:'pass'};
};
