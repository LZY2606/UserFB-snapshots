'use strict';
const assert=require('node:assert/strict');
const http=require('node:http');
const h2=require('node:http2');
const {once}=require('node:events');
const Crawler=require('../lib');
async function crawl(url, h2c=false){
 const c=new Crawler(url);c.respectRobotsTxt=false;c.interval=5;c.maxConcurrency=3;
 if(h2c)c.h2cOrigins=[new URL(url).origin];
 const data=[],errors=[];c.on('fetchcomplete',(q,b)=>data.push([q.path,b.toString()]));
 c.on('fetchclienterror',(q,e)=>errors.push(e.code));
 const done=once(c,'complete');c.start();
 try{await Promise.race([done,new Promise((_,reject)=>{const t=setTimeout(()=>reject(Error('crawl deadline')),5000);t.unref();})]);}
 finally{c.stop(true);c.httpAgent.destroy();c.httpsAgent.destroy();}
 return {data,errors};
}
(async()=>{
 const server=http.createServer((req,res)=>{res.setHeader('Content-Type','text/html');res.end(req.url==='/'?'<a href="/child">child</a>':'CHILD_OK');});
 server.listen(0,'127.0.0.1');await once(server,'listening');
 try{const r=await crawl('http://127.0.0.1:'+server.address().port+'/');assert.equal(r.errors.length,0);assert.equal(r.data.length,2);assert.equal(r.data.find(x=>x[0]==='/child')[1],'CHILD_OK');console.log('BASELINE_HTTP1_OK pages=2');}
 finally{await new Promise(r=>server.close(r));}
 if(process.argv[2]==='normal')return;
 const hserver=h2.createServer();hserver.on('sessionError',()=>{});hserver.on('stream',s=>{s.respond({':status':200,'content-type':'text/plain'});s.end('H2_ONLY');});
 hserver.listen(0,'127.0.0.1');await once(hserver,'listening');
 try{const r=await crawl('http://127.0.0.1:'+hserver.address().port+'/',true);assert.equal(r.data.length,0);assert.equal(r.errors.length,1);console.log('BASELINE_H2C_MISSING error='+r.errors[0]);}
 finally{await new Promise(r=>hserver.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1;});
