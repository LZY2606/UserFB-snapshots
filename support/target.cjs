const fs=require('node:fs');
if(!fs.existsSync('batch_collection_test.go')||!fs.readFileSync('batch_collection_test.go','utf8').trim()){console.error('Missing or empty batch_collection_test.go');process.exit(2)}
const {spawnSync}=require('node:child_process');
const p=spawnSync('go',['test','-mod=vendor','-count=1','-run','^Test','-json','.'],{encoding:'utf8'});
process.stdout.write(p.stdout||'');process.stderr.write(p.stderr||'');
if(p.status!==0)process.exit(p.status??1);
const records=(p.stdout||'').split('\n').filter(Boolean).map(x=>JSON.parse(x));
const done=records.filter(x=>x.Test?.startsWith('TestBatchCollection')&&!x.Test.includes('/')&&x.Action==='pass');
if(new Set(done.map(x=>x.Test)).size<3||records.some(x=>x.Test?.startsWith('TestBatchCollection')&&x.Action==='skip')){console.error('Need at least 3 passing, unskipped TestBatchCollection tests');process.exit(2)}
