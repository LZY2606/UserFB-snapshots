import assert from 'node:assert/strict';
import http from 'node:http';
import Crawler from '../dist/index.js';
const received=[],callbacks=[];
const server=http.createServer((req,res)=>{received.push(req.url);res.writeHead(200,{'content-type':'text/plain'});res.end('page:'+req.url);});
await new Promise(r=>server.listen(0,'127.0.0.1',r));
const base=`http://127.0.0.1:${server.address().port}`;
const c=new Crawler({maxConnections:1,priorityLevels:10,retries:0,jQuery:false,silence:true});
let releaseBlocker,ready,parallel;const first=new Promise(r=>ready=r),other=new Promise(r=>parallel=r);
const drained=new Promise(r=>c.once('drain',r));
const add=(path,priority,pool=1)=>c.add({url:base+path,priority,rateLimiterId:pool,callback:(error,response,done)=>{
 assert.equal(error,null);assert.equal(response.body,'page:'+path);callbacks.push(path);done();if(pool===2)parallel();
}});
const timer=setTimeout(()=>{throw new Error('scheduling baseline deadline');},10000);
try{
 c.add({url:base+'/blocker',rateLimiterId:1,callback:(e,r,done)=>{assert.equal(e,null);assert.equal(r.body,'page:/blocker');releaseBlocker=done;callbacks.push('/blocker');ready();}});
 await first;
 add('/alpha',2);add('/beta',2);add('/urgent',0);add('/parallel',1,2);
 await other;assert.deepEqual(received,['/blocker','/parallel']);releaseBlocker();await drained;
 const samePool=received.filter(p=>p!=='/parallel');
 assert.deepEqual(samePool,['/blocker','/urgent','/alpha','/beta'],'same-priority FIFO after reserved blocker and higher priority');
 assert.equal(callbacks.length,5);
 console.log('SCHEDULING_NORMAL_OK requests=5 pools=2 reserved-blocker=held priority=honored fifo=alpha,beta');
}finally{clearTimeout(timer);server.closeAllConnections();await new Promise(r=>server.close(r));}
