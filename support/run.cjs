const fs=require('node:fs');const path=require('node:path');const cp=require('node:child_process');
const os=require('node:os');const root=path.join(__dirname,'..');const mode=process.argv[2];
if(!['baseline','target'].includes(mode))throw Error('unknown mode');
let r=cp.spawnSync(process.execPath,['support/build.cjs'],{cwd:root,encoding:'utf8'});process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');if(r.status!==0)process.exit(r.status||1);
if(mode==='baseline') {r=cp.spawnSync(process.execPath,['support/baseline.cjs'],{cwd:root,encoding:'utf8',timeout:15000});process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');process.exit(r.status|| (r.error?1:0));}
const file=path.join(root,'test/encoded-reconnect.test.cjs');if(!fs.existsSync(file)||!fs.readFileSync(file,'utf8').trim())throw Error('missing or empty target tests');
const cov=fs.mkdtempSync(path.join(os.tmpdir(),'miniget-cover-'));
r=cp.spawnSync(process.execPath,['--test','--test-reporter=tap','--test-timeout=15000',file],{cwd:root,encoding:'utf8',timeout:20000,env:{...process.env,NODE_V8_COVERAGE:cov}});process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');if(r.status!==0)process.exit(r.status||1);
const output=r.stdout||'';const passed=new Set(output.split('\n').map(line=>{const m=/^ok \d+ - (.*)$/.exec(line);return m&&m[1];}));for(const name of ['encoded continuation','decoder terminal failures','paused acquisition'])if(!passed.has(name))throw Error('missing required passing test: '+name);
if(/# (?:SKIP|TODO)\b/.test(output)||!/# tests [1-9]\d*/.test(output))throw Error('zero/skipped target cases');

let reached=false;for(const f of fs.readdirSync(cov)){const c=JSON.parse(fs.readFileSync(path.join(cov,f),'utf8'));for(const script of c.result||[])if(script.url.endsWith('/dist/index.js'))for(const fn of script.functions||[])if(fn.functionName==='Miniget'&&fn.ranges.some(x=>x.count>0))reached=true;}fs.rmSync(cov,{recursive:true,force:true});if(!reached)throw Error('target tests did not invoke production Miniget');
