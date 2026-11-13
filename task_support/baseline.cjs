const assert=require('node:assert/strict');const Clock=require('../index');
const context={currentTime:0};const clock=new Clock(context,{tickMethod:'manual'});clock.start();
let deadlines=[];const e=clock.callbackAtTime(ev=>{deadlines.push(ev.deadline);if(deadlines.length===3)ev.clear()},1).repeat(.25);
for(const t of [.99,1,1.1,1.25,1.49,1.5]){context.currentTime=t;clock.tick()}
assert.deepEqual(deadlines,[1,1.25,1.5]);assert.equal(clock._hasEvent(e),false);
let delivered=0;const f=clock.callbackAtTime(()=>delivered++,3);clock.timeStretch(2,[f],2);assert.equal(f.deadline,4);
context.currentTime=4;clock.tick();assert.equal(delivered,1);
console.log('WAACLOCK_BASELINE callbacks=4 repeat_clear=1 stretched_deadline=4');
