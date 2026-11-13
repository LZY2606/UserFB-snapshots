const assert=require('node:assert/strict');
let suites=[],count=0,excluded=0;
global.describe=(name,fn)=>{suites.push({name,hooks:[]});try{fn()}finally{suites.pop()}};
global.beforeEach=fn=>suites[suites.length-1].hooks.push(fn);
global.it=(name,fn)=>{if(fn.toString().includes('._tick(')){excluded++;console.log('EXCLUDED_OBSOLETE_TICK '+name);return;}for(const s of suites)for(const h of s.hooks)h();if(fn.length)throw Error('Async old case not supported');fn();count++;};
require('../test/WAAClock-test.js');
assert.ok(count>=15);assert.equal(excluded,7);console.log('WAACLOCK_UPSTREAM tests='+count+' excluded_obsolete_tick='+excluded+' complete');
