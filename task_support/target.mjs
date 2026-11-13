import {spawnSync} from 'node:child_process';
import {statSync} from 'node:fs';
const file='test/node/service-location-abr.test.mjs';
try{if(!statSync(file).size)throw Error('empty test file');}catch(e){console.error(e.message);process.exit(1)}
const normal=spawnSync(process.execPath,['--import','./task_support/preload.mjs','--input-type=module','-e',"import {normal} from './task_support/normal.mjs'; normal()"],{encoding:'utf8'});process.stdout.write(normal.stdout||'');process.stderr.write(normal.stderr||'');if(normal.status!==0)process.exit(normal.status||1);
const p=spawnSync(process.execPath,['--import','./task_support/preload.mjs','--test','--test-reporter=tap',file],{encoding:'utf8'});process.stdout.write(p.stdout||'');process.stderr.write(p.stderr||'');
const names=['testServiceLocationHistory','testServiceLocationEwma','testServiceLocationAbr'];
if(p.status!==0||!/^# tests [1-9][0-9]*$/m.test(p.stdout)||!/^# skipped 0$/m.test(p.stdout)||!/^# todo 0$/m.test(p.stdout)||names.some(n=>!new RegExp('^ok [0-9]+ - '+n+'$','m').test(p.stdout)))process.exit(p.status||1);
console.log('service location target tests executed');
