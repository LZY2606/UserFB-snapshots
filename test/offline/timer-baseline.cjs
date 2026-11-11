const assert = require('node:assert/strict');
const createTimer = require('../../lib/timer');
function backend(disjoint) {
 let id=0; const ext={
 createQueryEXT(){return {id:++id};},beginQueryEXT(){},endQueryEXT(){},
 getQueryObjectEXT(q,p){return p===0x8867?true:2000000;},deleteQueryEXT(){}};
 return createTimer({getParameter(){return disjoint;}},{ext_disjoint_timer_query:ext});
}
const healthy=backend(false), good={gpuTime:0};
healthy.beginQuery(good);healthy.endQuery();healthy.update();
assert.equal(good.gpuTime,2);console.log('healthy query=2ms');
const invalid=backend(true), bad={gpuTime:0};
invalid.beginQuery(bad);invalid.endQuery();invalid.update();
assert.equal(bad.gpuTime,2);console.log('existing disjoint query still publishes 2ms');
const timer=backend(false), a={gpuTime:0}, b={gpuTime:0}, scope={gpuTime:0};
timer.beginQuery(a);timer.endQuery();timer.beginQuery(b);timer.endQuery();
timer.pushScopeStats(0,2,scope);timer.restore();
const fresh={gpuTime:0};timer.beginQuery(fresh);timer.endQuery();timer.update();
assert.equal(a.gpuTime,2);assert.equal(b.gpuTime,0);
assert.equal(scope.gpuTime,0);assert.equal(fresh.gpuTime,2);
console.log('existing restore contaminates old command and scope: a=2 b=0 scope=0; fresh=2');
