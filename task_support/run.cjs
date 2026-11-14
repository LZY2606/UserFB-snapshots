'use strict';
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {spawnSync}=require('node:child_process');
process.chdir(path.resolve(__dirname,'..'));
fs.mkdirSync('.task-build/tmp',{recursive:true});
const mode=process.argv[2]||'baseline';
function run(args){
 const result=spawnSync(process.execPath,args,{encoding:'utf8',timeout:120000,maxBuffer:4*1024*1024,env:{...process.env,NODE_PATH:'',NODE_OPTIONS:'',NODE_DISABLE_COMPILE_CACHE:'1',TMPDIR:path.resolve('.task-build/tmp')}});
 if(result.stdout)process.stdout.write(result.stdout);if(result.stderr)process.stderr.write(result.stderr);
 if(result.error)throw result.error;
 if(result.status!==0)process.exit(result.status||1);
}
run(['task_support/build.cjs']);
if(mode==='baseline')run(['task_support/baseline.mjs']);
else if(mode==='target'){
 assert.ok(fs.existsSync('test/revision-target.cjs')&&fs.statSync('test/revision-target.cjs').size>0,'missing/empty target');
 fs.rmSync('.task-build/report.json',{force:true});run(['task_support/execute.cjs']);
 assert.ok(fs.existsSync('.task-build/report.json'),'no completed report');
 const report=JSON.parse(fs.readFileSync('.task-build/report.json','utf8'));
 assert.deepEqual(report.map(r=>r.name),['revisionPending','revisionInFlight','revisionRetry','revisionRelease']);
 assert.ok(report.every(r=>r.status==='pass'&&r.assertions>0),'zero checks or failed group');
 console.log('TARGET_WRAPPER_OK');
}else throw new Error('use baseline or target');
