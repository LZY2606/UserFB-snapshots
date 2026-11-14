const assert=require('node:assert/strict');
const http=require('node:http');
const fs=require('node:fs');
const path=require('node:path');
const os=require('node:os');
const {spawn}=require('node:child_process');
(async()=>{
 const out=fs.mkdtempSync(path.join(os.tmpdir(),'z2027-baseline-'));
 const server=http.createServer((req,res)=>{
  if(req.url==='/robots.txt'){res.writeHead(200,{'Content-Type':'text/plain'});res.end('User-agent: *\nDisallow:\n');return;}
  res.writeHead(200,{'Content-Type':'text/html','Trailer':'X-Crawl-Cursor','Connection':'close'});
  res.write('<html><body>TRAILER_BODY_OK</body></html>');
  res.addTrailers({'X-Crawl-Cursor':'cursor-z2027-distinct'});res.end();
 });
 await new Promise(r=>server.listen(0,'127.0.0.1',r));
 try {
  const url=`http://127.0.0.1:${server.address().port}/index.html`;
  const child=spawn(path.resolve('.build/engine/src/httrack'),[url,'-O',out,'--quiet','--depth=1','--robots=0','--max-time=15','--timeout=5','--retries=0','--disable-security-limits'],{stdio:['ignore','pipe','pipe']});
  let log='';child.stdout.on('data',x=>log+=x);child.stderr.on('data',x=>log+=x);
  const code=await new Promise((resolve,reject)=>{child.on('error',reject);child.on('exit',resolve)});
  process.stdout.write(log);assert.equal(code,0);
  const files=[];function walk(d){for(const n of fs.readdirSync(d)){const f=path.join(d,n);if(fs.statSync(f).isDirectory())walk(f);else files.push(f)}}walk(out);
  assert(files.some(f=>f.endsWith('index.html')&&fs.readFileSync(f).includes(Buffer.from('TRAILER_BODY_OK'))));
  const z=fs.readFileSync(path.join(out,'hts-cache/new.zip'));
  assert(z.includes(Buffer.from('Content-Type: text/html')));
  assert(!z.includes(Buffer.from('cursor-z2027-distinct')));
  console.log('BASELINE body=ok cache_initial_metadata=present trailer_metadata=absent');
 } finally {await new Promise(r=>server.close(r));fs.rmSync(out,{recursive:true,force:true});}
})().catch(e=>{console.error(e);process.exitCode=1});
