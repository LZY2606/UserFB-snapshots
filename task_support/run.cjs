'use strict';
const fs=require('node:fs'),path=require('node:path'),{spawnSync}=require('node:child_process');
process.chdir(path.resolve(__dirname,'..'));
function execute(args){const r=spawnSync(process.execPath,args,{encoding:'utf8',timeout:90000,env:{...process.env,NODE_PATH:'',NODE_OPTIONS:''}});process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');if(r.error)throw r.error;if(r.status!==0)process.exit(r.status||1);return r.stdout;}
if(process.argv[2]==='baseline'){execute(['task_support/baseline.cjs']);}
else if(process.argv[2]==='target'){
 execute(['task_support/baseline.cjs','normal']);
 const target='test/h2c-target.cjs';if(!fs.existsSync(target)||!fs.readFileSync(target,'utf8').trim())throw Error('missing target');
 const output=execute(['task_support/check.cjs',target]);
 for(const group of ['H2Session','H2Goaway','H2Crawl'])if(!output.includes('GROUP_RETURNED '+group+' checks='))throw Error('group did not return '+group);
 if(!output.includes('ALL_GROUPS_RETURNED 3'))throw Error('missing completion receipt');
}else throw Error('use baseline or target');
