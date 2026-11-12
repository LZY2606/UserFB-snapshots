const fs=require('node:fs'),assert=require('node:assert/strict');
const t=fs.readFileSync(process.argv[2],'utf8');
for(const g of ['parse','anchors','chain','path']) assert(new RegExp('^ok \\d+ - gpos '+g+'(?: |$)','m').test(t),'missing executed gpos '+g);
assert(/^# pass [1-9]\d*$/m.test(t)); assert(/^# fail 0$/m.test(t)); assert(/^# skipped 0$/m.test(t));
console.log('TYPR_GPOS_TARGET_OK');
