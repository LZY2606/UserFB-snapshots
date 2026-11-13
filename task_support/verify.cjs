const fs=require('node:fs');const s=fs.readFileSync(process.argv[2],'utf8');
for(const group of ['normal','boundary','integration']){
 if(!new RegExp('^GROUP '+group+' CASES [1-9][0-9]* CHECKS [1-9][0-9]* COMPLETE$','m').test(s))throw Error('incomplete '+group);
}
if(!/^TARGET_ALL_COMPLETE$/m.test(s))throw Error('early termination');
console.log('TARGET_GROUPS_COMPLETE');
