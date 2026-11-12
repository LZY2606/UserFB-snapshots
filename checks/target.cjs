const {spawnSync} = require('node:child_process');
const fs = require('node:fs');
const file='test/binary-transcoder.test.cjs';
if(!fs.existsSync(file)) throw Error('missing '+file);
const r=spawnSync(process.execPath,['--test','--test-reporter=tap',file],{encoding:'utf8'});
process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');
if(r.status!==0) process.exit(1);
for(const name of ['binary-record','binary-union','binary-blocks','binary-framing']) {
 const escaped=name.replace(/[.*+?^${}()|[\]\\]/g,'\\$&');
 if(!new RegExp('^ok [0-9]+ - '+escaped+'(?:\\s|$)','m').test(r.stdout)) throw Error('unexecuted group '+name);
}
if(!/^# tests [1-9][0-9]*$/m.test(r.stdout)||!/^# skipped 0$/m.test(r.stdout)||!/^# cancelled 0$/m.test(r.stdout)||!/^# todo 0$/m.test(r.stdout)) throw Error('empty or skipped suite');
const b=spawnSync(process.execPath,['checks/baseline.cjs'],{stdio:'inherit'}); if(b.status!==0) process.exit(1);
