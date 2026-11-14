'use strict';
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const cases=require(path.resolve(process.argv[2]));
 for(const name of ['TunnelTransport','RedirectIdentity','TunnelLifecycle']){
  assert.equal(typeof cases[name],'function','missing '+name);let count=0,active=true;
  const c={check(value,message){assert(active,'late check');assert(value,message);count++;}};
  await cases[name](c);active=false;assert(count>=3,'no content assertion '+name);
  console.log('GROUP_RETURNED '+name+' checks='+count);
 }
 console.log('ALL_GROUPS_RETURNED 3');
})().catch(e=>{console.error(e);process.exitCode=1;});
