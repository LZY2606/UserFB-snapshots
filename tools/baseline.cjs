const assert=require('node:assert/strict');
const loadSource=require('../tests/helpers/load-source.cjs');
const ops=[];
class Buffer {
 constructor(){this.updating=false;this.e={};this.buffered={length:0};}
 addEventListener(n,f){this.e[n]=f;} removeEventListener(){}
 appendBuffer(b){assert.equal(this.updating,false);ops.push(['append',b[0]]);this.updating=true;}
 changeType(m){ops.push(['changeType',m]);}
 finish(){this.updating=false;this.e.updateend();}
}
class MediaSource {
 constructor(){this.readyState='open';this.buffers=[];}
 addEventListener(){} removeEventListener(){}
 addSourceBuffer(m){ops.push(['create',m]);const b=new Buffer();this.buffers.push(b);return b;}
}
const C=loadSource({navigator:{userAgent:'Chrome/120.0.0.0'},self:{MediaSource}})('core/mse-controller.js').default;
const mse=new C({autoCleanupSourceBuffer:false});mse.initialize({getCurrentTime:()=>0,getReadyState:()=>1});
const init=(codec,n)=>({type:'video',container:'video/mp4',codec,data:new Uint8Array([n])});
mse.appendInitSegment(init('avc1.42E01E',1));
mse.appendMediaSegment({type:'video',data:new Uint8Array([2])});
const sb=mse.getObject().buffers[0];sb.finish();sb.finish();
assert.deepEqual(ops.filter(x=>x[0]==='append').map(x=>x[1]),[1,2]);
console.log('BASELINE_MSE_INIT_MEDIA_ORDER_OK');
mse.appendInitSegment(init('hvc1.1.6.L93.B0',3));sb.finish();
assert.equal(ops.filter(x=>x[0]==='changeType').length,0);
assert.equal(ops.at(-1)[1],3);
console.log('BASELINE_MSE_CODEC_CHANGE_ONLY_APPENDS');
