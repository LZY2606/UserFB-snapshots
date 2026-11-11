const {test}=require('node:test');
const assert=require('node:assert/strict');
const Crawler=require('../lib');
function fixture(){
 const c=new Crawler('http://example.test/');c.respectRobotsTxt=false;c.running=true;
 const events=[];c.on('complete',()=>events.push('complete'));
 c.on('queueadd',i=>events.push('add:'+i.path));
 c.fetchQueueItem=i=>{i.status='spooled';};
 return {c,events};
}
function settle(c){for(const i of c.queue){i.fetched=true;i.status='downloaded';}c.crawl();}
test('admission belongs to unfinished work until its decision settles',()=>{
 const {c,events}=fixture();let release;
 c.addFetchCondition((_i,_r,done)=>{release=done;});
 c.queueURL('http://example.test/delayed');c.crawl();
 assert.deepEqual(events,[]);
 release(null,true);assert.deepEqual(events,['add:/delayed']);
 c.crawl();assert.deepEqual(events,['add:/delayed']);
 settle(c);assert.deepEqual(events,['add:/delayed','complete']);
});
test('rejected, errored and synchronous conditions leave no phantom work',()=>{
 for(const mode of ['reject','error','sync']){
  const {c,events}=fixture();let release;
  c.on('fetchconditionerror',()=>events.push('conditionerror'));
  if(mode==='sync') c.addFetchCondition(()=>false);
  else c.addFetchCondition((_i,_r,done)=>{release=done;});
  c.queueURL('http://example.test/drop');
  if(mode!=='sync'){c.crawl();assert.deepEqual(events,[]);release(mode==='error'?new Error('fixture'):null,false);}
  c.crawl();assert.equal(events.at(-1),'complete');
  assert.equal(events.filter(x=>x==='complete').length,1);
 }
});
test('async queue.add remains unfinished after conditions accept',()=>{
 const {c,events}=fixture();let commit;
 const add=c.queue.add;
 c.queue.add=function(item,force,done){commit=()=>add.call(this,item,force,done);};
 c.queueURL('http://example.test/storage');c.crawl();assert.deepEqual(events,[]);
 commit();c.queue.add=add;c.crawl();assert.deepEqual(events,['add:/storage']);
 settle(c);assert.deepEqual(events,['add:/storage','complete']);
});
test('old equal counts cannot certify an empty queue after new admission',()=>{
 const {c,events}=fixture();let releaseLength;
 const getLength=c.queue.getLength;
 c.queue.getLength=function(done){const length=this.length;releaseLength=()=>done(null,length);};
 c.crawl();assert.equal(typeof releaseLength,'function');
 c.queue.getLength=getLength;
 c.queueURL('http://example.test/new');
 releaseLength();
 assert.deepEqual(events,['add:/new']);
 settle(c);assert.deepEqual(events,['add:/new','complete']);
});
test('queue error and duplicate are terminal admission outcomes',()=>{
 for(const code of ['DUPLICATE','STORE']){
  const {c,events}=fixture();let fail;
  c.on('queueerror',()=>events.push('queueerror'));
  c.queue.add=(_i,_f,done)=>{fail=()=>done(Object.assign(new Error('fixture'),{code}));};
  c.queueURL('http://example.test/fail');c.crawl();assert.deepEqual(events,[]);
  fail();c.crawl();assert.equal(events.at(-1),'complete');
 }
});
