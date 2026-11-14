"use strict";
const path=require('node:path');
const names=['geometry_identity','edit_topology','normal_closure','sparse_replay'];
const target=path.resolve(__dirname,'../test/incremental.js');
const tests=require(target);
if(!tests||typeof tests!=='object'||Array.isArray(tests)||Object.keys(tests).length!==names.length||names.some(n=>typeof tests[n]!=='function'))throw Error('exactly four named test functions required');
const protocol=console.log.bind(console);
for(const name of names){
 let count=0,failed=false,skipped=false;
 const c=Object.freeze({check(value,message='check failed'){
  count++; if(value!==true){failed=true;throw Error(message);}
 },skip(){skipped=true;failed=true;throw Error('skip forbidden');}});
 let result;
 try{result=tests[name](c);}catch(e){failed=true;console.error(`${name}: ${e.stack||e}`);}
 if(result!==undefined||failed||skipped||count===0)throw Error(`${name}: failed/skip/empty/non-sync test`);
 protocol('TASK_GROUP '+name+' '+count);
}
protocol('TASK_ALL_COMPLETE');
