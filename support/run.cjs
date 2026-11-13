const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const cp = require('node:child_process');
const mode = process.argv[2];
const cases = JSON.parse(fs.readFileSync(path.join(__dirname,'cases.json'),'utf8'))[mode];
if (!Array.isArray(cases) || !cases.length) throw Error('unknown mode or zero required tests');
const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'go-'));
const env={...process.env,GOMODCACHE:path.join(tmp,'mod'),GOCACHE:path.join(tmp,'build'),GOPROXY:'off',GOSUMDB:'off',GOTOOLCHAIN:'local',GOFLAGS:'-mod=vendor'};
const r=cp.spawnSync('go',['test','-json','-count=1','-timeout=90s','-coverprofile='+path.join(tmp,'production.cover'),'-run','^('+cases.join('|')+')$','.'],{cwd:path.join(__dirname,'..'),env,encoding:'utf8',maxBuffer:32*1024*1024});
process.stdout.write(r.stdout||''); process.stderr.write(r.stderr||'');
let pass=new Set(),bad=new Set();
for (const line of (r.stdout||'').split('\n')) {try {const e=JSON.parse(line);if(e.Test&&e.Action==='pass')pass.add(e.Test);if(e.Test&&['skip','fail'].includes(e.Action))bad.add(e.Test);}catch{}}
const coverage=fs.existsSync(path.join(tmp,'production.cover'))?fs.readFileSync(path.join(tmp,'production.cover'),'utf8'):'';
fs.rmSync(tmp,{recursive:true,force:true});
if(r.error) {console.error(r.error);process.exit(1);}
if(r.status!==0) process.exit(r.status||1);
for(const name of cases) if(!pass.has(name)||bad.has(name)) {console.error('required test did not run and pass: '+name);process.exit(1);}
if(bad.size) {console.error('skipped/failed cases: '+[...bad]);process.exit(1);}
console.log('required tests passed: '+cases.join(', '));

const regions=JSON.parse(fs.readFileSync(path.join(__dirname,'production.json'),'utf8'));
const executed=coverage.split('\n').map(line=>{const m=/^(.+):(\d+)\.\d+,(\d+)\.\d+ \d+ (\d+)$/.exec(line);return m?{file:m[1],start:+m[2],end:+m[3],count:+m[4]}:null}).filter(x=>x&&x.count>0&&!x.file.endsWith('_test.go'));
if(!executed.length)throw Error('no executed production code');
for(const region of regions)if(!executed.some(x=>x.file.endsWith('/'+region.path)&&x.start<=region.end&&x.end>=region.start))throw Error('production path not exercised: '+JSON.stringify(region));
