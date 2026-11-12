const fs=require('node:fs'),vm=require('node:vm');
const box={console:{log(){},warn:console.warn,error:console.error},TextDecoder,TextEncoder,Uint8Array,Uint16Array,Int16Array,Uint32Array,Int32Array,Float32Array,Float64Array,ArrayBuffer,DataView};
box.window=box; vm.createContext(box);
for(const f of ['src/Typr.js','src/Typr.U.js']) vm.runInContext(fs.readFileSync(f,'utf8'),box,{filename:f});
module.exports=box.Typr;
