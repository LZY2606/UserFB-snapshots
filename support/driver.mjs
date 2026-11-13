import assert from 'node:assert/strict';
import {registerCases} from './target.mjs';
const cases=[];
registerCases({add(group,name,fn){assert.equal(typeof fn,'function');cases.push({group,name,fn});}});
for(const group of ['position','normal','records','ownership']){
 const selected=cases.filter(c=>c.group===group);assert.ok(selected.length>0,`empty ${group}`);
 let checks=0;
 for(const item of selected)await item.fn({check(ok,message){checks++;assert.ok(ok,`${group}/${item.name}: ${message}`);}});
 assert.ok(checks>=5,`too few assertions ${group}: ${checks}`);
 console.log(`EXECUTED ${group} cases=${selected.length} assertions=${checks}`);
}
console.log('TARGET_OK');
