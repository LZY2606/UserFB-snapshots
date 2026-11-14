'use strict';
const fs=require('node:fs');
const path=require('node:path');
const {spawnSync}=require('node:child_process');
process.chdir(path.resolve(__dirname,'..'));
const env={...process.env,NODE_PATH:path.resolve('task_support/vendor'),NODE_OPTIONS:''};
function run(args) {
 const r=spawnSync(process.execPath,args,{env,encoding:'utf8',timeout:90000,maxBuffer:4*1024*1024});
 process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');
 if(r.error)throw r.error;
 if(r.status!==0)process.exit(r.status||1);
 return r.stdout;
}
if(!['baseline','target'].includes(process.argv[2]))throw Error('use baseline or target');
run(['task_support/baseline.cjs']);
if(process.argv[2]==='target') {
 const file='test/origin-scheduler.cjs';
 if(!fs.existsSync(file)||!fs.readFileSync(file,'utf8').trim())throw Error('missing target tests');
 const out=run(['--test','--test-reporter=tap',file]);
 for(const group of ['Origin fairness','Robots isolation','Async queue lifecycle'])
  if(!out.includes('# Subtest: '+group))throw Error('missing test group '+group);
 const count=name=>Number((out.match(new RegExp('^# '+name+' (\\d+)$','m'))||[])[1]);
 if(!(count('tests')>=3&&count('pass')===count('tests')&&count('fail')===0&&count('skipped')===0&&count('todo')===0))
  throw Error('incomplete or skipped target tests');
}
