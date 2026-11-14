"use strict";
const fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const root=path.resolve(__dirname,'..'),target=path.join(root,'test/incremental.js');
function fail(msg){console.error(msg);process.exit(1);}
if(!fs.existsSync(target)||fs.statSync(target).size===0)fail('target missing or empty');
const syntax=cp.spawnSync(process.execPath,['--check',target],{cwd:root,encoding:'utf8'});
if(syntax.status!==0)fail(syntax.stderr||'syntax check failed');
const run=cp.spawnSync(process.execPath,[path.join(__dirname,'worker.js')],{cwd:root,encoding:'utf8',timeout:30000,maxBuffer:8*1024*1024});
process.stdout.write(run.stdout||'');process.stderr.write(run.stderr||'');
if(run.error||run.status!==0)fail('target execution failed: '+(run.error||run.status));
const lines=(run.stdout||'').trim().split(/\r?\n/);
const names=['geometry_identity','edit_topology','normal_closure','sparse_replay'];
for(const name of names){const hits=lines.filter(s=>new RegExp('^TASK_GROUP '+name+' [1-9][0-9]*$').test(s));if(hits.length!==1)fail('missing/duplicate content checks: '+name);}
if(lines.filter(s=>s==='TASK_ALL_COMPLETE').length!==1)fail('early exit or incomplete execution');
console.log('TARGET_WRAPPER_COMPLETE');
