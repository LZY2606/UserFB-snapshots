const fs=require('node:fs'),assert=require('node:assert/strict');const t=fs.readFileSync(process.argv[2],'utf8');
for(const g of ['parse','variant','assembly','path']) assert(new RegExp('^ok \\d+ - math '+g+'(?: |$)','m').test(t),'missing executed math '+g);
assert(/^# pass [1-9]\d*$/m.test(t));assert(/^# fail 0$/m.test(t));assert(/^# skipped 0$/m.test(t));
console.log('OPENTYPE_MATH_TARGET_OK');
