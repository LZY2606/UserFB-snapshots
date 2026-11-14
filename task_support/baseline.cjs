'use strict';
const assert=require('node:assert/strict'),http=require('node:http'),fs=require('node:fs');
const {once}=require('node:events');const Crawler=require('../');const Queue=require('../lib/queue');
const requests=[],delivered=[];
const server=http.createServer((req,res)=>{requests.push(req.url);res.setHeader('Content-Type','text/html');
 if(req.url==='/root')res.end('<a href="/branch">one</a><a href="/jump">two</a><a href="/branch">again</a>');
 else if(req.url==='/jump'){res.writeHead(302,{Location:'/leaf'});res.end();}
 else res.end('collected:'+req.url);
});
(async()=>{
 server.listen(0,'127.0.0.1');await once(server,'listening');
 const c=new Crawler(`http://127.0.0.1:${server.address().port}/root`);c.respectRobotsTxt=false;c.interval=2;
 c.on('fetchcomplete',(q,b)=>delivered.push([q.path,b.toString()]));
 const timer=setTimeout(()=>{throw new Error('baseline deadline');},10000);
 try{
  const done=once(c,'complete');c.start();await done;
  assert.deepEqual([...requests].sort(),['/branch','/jump','/leaf','/root']);
  assert.deepEqual(delivered.map(x=>x[0]).sort(),['/branch','/leaf','/root']);
  assert.ok(delivered.some(x=>x[1]==='collected:/leaf'));assert.equal(c._openRequests.length,0);
  const before=JSON.parse(JSON.stringify(c.queue));
  await new Promise((ok,no)=>c.queue.freeze('.task-build/normal-queue.json',e=>e?no(e):ok()));
  const q=new Queue();await new Promise((ok,no)=>q.defrost('.task-build/normal-queue.json',e=>e?no(e):ok()));
  assert.deepEqual(JSON.parse(JSON.stringify(q)),before);assert.equal(q.length,4);
  let duplicate;q.exists(c.initialURL,(_e,v)=>duplicate=v);assert.equal(duplicate,1);
  console.log('CRAWL_RESUME_BASELINE_OK pages=3 redirect=1 frozen=4 dedupe=preserved');
 }finally{clearTimeout(timer);c.stop(true);c.httpAgent.destroy();c.httpsAgent.destroy();server.closeAllConnections();await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1;});
