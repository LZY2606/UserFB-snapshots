'use strict';
const fs=require('node:fs'),path=require('node:path');
(async()=>{
 const target=require(path.resolve('test/revision-target.cjs'));
 const groups=['revisionPending','revisionInFlight','revisionRetry','revisionRelease'];
 const run=require('./check.cjs'),report=[];
 for(const name of groups){
  report.push(await run(name,target[name]));
  console.log('GROUP_OK '+name);
 }
 fs.writeFileSync('.task-build/report.json',JSON.stringify(report));
})().catch(error=>{console.error(error.stack||error);process.exitCode=1;});
