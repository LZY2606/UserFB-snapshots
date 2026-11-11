import assert from 'node:assert/strict';
import http from 'node:http';
import {mkdtemp, readFile, readdir, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import scrape from '../index.mjs';
const hits=[];
const files={
 '/index.html':['text/html','<link rel="stylesheet" href="/a.css"><script type="module" src="/main.js"></script>'],
 '/a.css':['text/css','body{background:url(/pixel.svg)}'],
 '/pixel.svg':['image/svg+xml','<svg xmlns="http://www.w3.org/2000/svg"/>'],
 '/main.js':['application/javascript','import {value} from "./dep.js"; export const result=value;'],
 '/dep.js':['application/javascript','export const value=17;']
};
const server=http.createServer((req,res)=>{
 hits.push(req.url); const f=files[req.url];
 res.writeHead(f?200:404,{'content-type':f?.[0]??'text/plain'}); res.end(f?.[1]??'missing');
});
const base=await mkdtemp(join(tmpdir(),'site-baseline-'));
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
try{
 const directory=join(base,'saved');
 const result=await scrape({urls:[`http://127.0.0.1:${server.address().port}/index.html`],directory});
 assert.equal(result.length,1);
 const html=await readFile(join(directory,'index.html'),'utf8');
 assert.match(html,/js\/main.js/);
 assert.match(await readFile(join(directory,'css/a.css'),'utf8'),/\.\.\/fonts\/pixel.svg/);
 assert.equal(await readFile(join(directory,'js/main.js'),'utf8'),files['/main.js'][1]);
 assert.deepEqual([...hits].sort(),['/a.css','/index.html','/main.js','/pixel.svg']);
 assert.deepEqual(await readdir(join(directory,'js')),['main.js']);
 console.log('BASELINE_ASSET_GRAPH_OK module-dependency-requests=0');
}finally{await new Promise(resolve=>server.close(resolve)); await rm(base,{recursive:true,force:true});}
