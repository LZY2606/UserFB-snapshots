const assert=require('node:assert/strict');const http=require('node:http');const zlib=require('node:zlib');const miniget=require('../dist');
(async()=>{
 const plain=Buffer.alloc(128*1024);let n=0x12345678;for(let i=0;i<plain.length;i++){n^=n<<13;n^=n>>>17;n^=n<<5;plain[i]=n&255;}
 const wire=zlib.gzipSync(plain);let broken;const ranges=[];
 const server=http.createServer((req,res)=>{
  res.setHeader('Content-Encoding','gzip');res.setHeader('Accept-Ranges','bytes');
  if(req.url==='/normal'){res.setHeader('Content-Length',wire.length);res.end(wire);return;}
  if(!req.headers.range){res.setHeader('Content-Length',wire.length);broken=res;res.write(wire.subarray(0,Math.floor(wire.length/2)));return;}
  ranges.push(req.headers.range);const offset=Number(/^bytes=(\d+)-$/.exec(req.headers.range)[1]);
  res.statusCode=206;res.setHeader('Content-Range',`bytes ${offset}-${wire.length-1}/${wire.length}`);res.setHeader('Content-Length',wire.length-offset);res.end(wire.subarray(offset));
 });
 await new Promise(r=>server.listen(0,'127.0.0.1',r));
 const base='http://127.0.0.1:'+server.address().port;
 try {
  const opts={acceptEncoding:{gzip:()=>zlib.createGunzip()},maxRetries:0,maxReconnects:1,backoff:{inc:1,max:1}};
  const normal=miniget(base+'/normal',{...opts,maxReconnects:0});const parts=[];await new Promise((resolve,reject)=>{normal.on('data',b=>parts.push(b));normal.once('end',resolve);normal.once('error',reject);});
  assert.deepEqual(Buffer.concat(parts),plain);console.log('normal compressed HTTP capture: decoded_bytes='+plain.length);
  const s=miniget(base+'/broken',opts);let output=0;let reconnects=0;
  s.on('response',r=>{if(r.statusCode===200)r.once('data',()=>setImmediate(()=>broken.destroy()));});
  s.on('reconnect',()=>reconnects++);s.on('data',b=>output+=b.length);
  const err=await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('missing terminal error')),3000);s.on('error',e=>{console.log('old stream error: '+e.code+' '+e.message);if(e.code==='Z_DATA_ERROR'){clearTimeout(timer);resolve(e);}});s.once('end',()=>{clearTimeout(timer);reject(Error('unexpected success'));});});
  assert.equal(err.code,'Z_DATA_ERROR');assert.match(err.message,/incorrect header check/);assert.equal(reconnects,1);assert.equal(ranges.length,1);assert.ok(output>0&&output<plain.length);
  console.log('observed compressed reconnect failure: '+err.code+' '+err.message+' reconnects='+reconnects+' range='+ranges[0]+' decoded_prefix='+output);
  s.destroy();
 }finally{server.closeAllConnections();await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1;});
