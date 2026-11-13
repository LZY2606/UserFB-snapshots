const fs=require('node:fs'),{spawnSync}=require('node:child_process');
const file="core/spider/checkpoint_test.go",prefix="TestCheckpoint";
if(!fs.existsSync(file)||!fs.readFileSync(file,'utf8').trim()){console.error('required regression file missing/empty: '+file);process.exit(2);}
const r=spawnSync('go',['test','-vet=off','-count=1','-timeout=120s','-json',...["./core/spider", "./core/scheduler"]],{encoding:'utf8',maxBuffer:32*1024*1024});
process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');
if(r.error){console.error(r.error);process.exit(2);}if(r.status!==0)process.exit(r.status||2);
const passed=new Set();let skipped=false;
for(const line of r.stdout.split('\n')){let e;try{e=JSON.parse(line)}catch{continue}if(e.Test&&e.Test.startsWith(prefix)){if(e.Action==='skip')skipped=true;if(e.Action==='pass'&&!e.Test.includes('/'))passed.add(e.Test);}}
if(skipped||passed.size<3){console.error('need >=3 distinct non-skipped top-level '+prefix+' tests');process.exit(2);}
