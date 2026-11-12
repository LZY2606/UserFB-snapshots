const {spawnSync}=require('node:child_process');
const fs=require('node:fs');
const file="test/indexed-acquisition-join.cjs";
if(!fs.existsSync(file)){console.error('Missing target test: '+file);process.exit(1);}
const r=spawnSync(process.execPath,['--test','--test-reporter=tap',file],{encoding:'utf8'});
process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');
const out=r.stdout||'';
function total(n){const m=out.match(new RegExp('^# '+n+' (\\d+)$','m'));return m?Number(m[1]):-1;}
const groups=["ordered-join", "tuple-context", "fallback-semantics", "linear-work"];
if(r.status!==0||total('tests')<groups.length||total('pass')!==total('tests')||['fail','cancelled','skipped','todo'].some(x=>total(x)!==0)||groups.some(g=>!out.includes('# Subtest: '+g+'\n'))){console.error('Target test execution contract failed');process.exit(1);}
const b=spawnSync(process.execPath,['harness/baseline.cjs'],{encoding:'utf8'});
process.stdout.write(b.stdout||'');process.stderr.write(b.stderr||'');
if(b.status!==0)process.exit(1);
console.log('TARGET_OK');
