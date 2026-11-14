const net=require('node:net');
const {spawn}=require('node:child_process');
const path=require('node:path');
const server=net.createServer(socket=>{
 let request='';let sent=false;
 socket.on('error',()=>{});
 socket.on('data',b=>{
  if(sent)return;request+=b.toString('latin1');if(!request.includes('\r\n\r\n'))return;sent=true;
  const plain=request.startsWith('GET /plain ');
  const header='HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n'+(plain?'':'Trailer: X-Crawl-Cursor, X-Crawl-Note\r\n')+'\r\n';
  socket.end(header+'3\r\nabc\r\n0\r\n'+(plain?'':'X-Crawl-Cursor: page-2\r\nX-Crawl-Note: first\r\nX-Crawl-Note: second\r\n')+'\r\n');
 });
});
server.listen(0,'127.0.0.1',()=>{
 const child=spawn(path.resolve(process.argv[2]),process.argv.slice(3),{stdio:'inherit',env:{...process.env,UFB_TRAILER_BASE:`http://127.0.0.1:${server.address().port}`}});
 child.on('error',e=>{console.error(e);server.close();process.exitCode=1});
 child.on('exit',(code,signal)=>{server.close();process.exitCode=signal?1:code});
});
