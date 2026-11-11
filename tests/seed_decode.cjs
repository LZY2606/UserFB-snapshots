const fs=require('fs'),vm=require('vm'),assert=require('assert'),crypto=require('crypto');
const ctx={window:{performance:{now:()=>0}},document:{readyState:'loading',addEventListener(){}}};
vm.createContext(ctx);
for(const f of ['jsmpeg.js','buffer.js','decoder.js','mpeg1.js'])
  vm.runInContext(fs.readFileSync('src/'+f,'utf8'),ctx);
const d=new ctx.JSMpeg.Decoder.MPEG1Video({decodeFirstFrame:false});
const hashes=[];
d.connect({resize(w,h){assert.equal(w,48);assert.equal(h,32);},render(y){
  hashes.push(crypto.createHash('sha256').update(y).digest('hex').slice(0,12));
}});
const raw=Buffer.from(fs.readFileSync('tests/fixtures/motion.m1v.b64','utf8'),'base64');
d.write(0,[raw]);
while(d.decode()){}
assert.deepEqual(hashes,['99c64e421c64','5b4edfc57d16','60757c9b8f64','1318db64e1a4','7c5a2b2492ac','f0833fad2e58']);
console.log('six-frame pixel baseline PASS',hashes.join(','));
