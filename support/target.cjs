const assert=require('node:assert/strict');
const fs=require('node:fs');const os=require('node:os');const path=require('node:path');const {spawnSync}=require('node:child_process');
const {bundle,environment}=require('./build.cjs');
assert.ok(fs.existsSync('tests/drag-contract.ts'),'Add tests/drag-contract.ts');
const temp=fs.mkdtempSync(path.join(os.tmpdir(),'user-event-drag-'));
try{
 const file=path.join(temp,'drag-contract.cjs');bundle('tests/drag-contract.ts',file);
 const r=spawnSync(process.execPath,['--test','--test-reporter=tap',file],{encoding:'utf8',env:environment});
 process.stdout.write(r.stdout);process.stderr.write(r.stderr);assert.equal(r.status,0);
 assert.match(r.stdout,/^# tests [1-9][0-9]*$/m);assert.match(r.stdout,/^# pass [1-9][0-9]*$/m);assert.match(r.stdout,/^# skipped 0$/m);
 assert.match(r.stdout,/^ok [0-9]+ - drag session contract$/m);
 for(const marker of ['DRAG_ROUTE_OK','DRAG_STORE_OK','DRAG_EFFECT_OK','DRAG_STATE_OK'])assert.ok(r.stdout.includes(marker),marker);
 const legacy=spawnSync(process.execPath,['support/baseline.cjs'],{encoding:'utf8',env:environment});
 process.stdout.write(legacy.stdout);process.stderr.write(legacy.stderr);assert.equal(legacy.status,0);
 console.log('DRAG_TARGET_OK');
}finally{fs.rmSync(temp,{recursive:true,force:true});}
