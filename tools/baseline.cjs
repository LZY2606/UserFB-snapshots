const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const nodes=[];let wall=10;
class AudioContext {
 constructor(){this.currentTime=10;this.destination={};}
 createGain(){return {gain:{value:1},connect(){},disconnect(){}};}
 createBuffer(c,n,rate){return {duration:n/rate,getChannelData(){return new Float32Array(n)}};}
 createBufferSource(){const n={connect(){},start(t){this.at=t},stop(){this.stopped=true},disconnect(){}};nodes.push(n);return n;}
 close(){}
}
const c={JSMpeg:{AudioOutput:{},Now:()=>wall},window:{AudioContext},navigator:{userAgent:'desktop'},setTimeout};
vm.createContext(c);vm.runInContext(fs.readFileSync('src/webaudio.js','utf8'),c);
const out=new c.JSMpeg.AudioOutput.WebAudio({});
out.play(100,new Float32Array(100).fill(.25),new Float32Array(100).fill(-.25));
assert.equal(nodes[0].at,10);assert.equal(out.enqueuedTime,1);
console.log('BASELINE_AUDIO_PCM_SCHEDULE_OK');
out.stop();assert.equal(out.gain.gain.value,0);
assert.equal(nodes[0].stopped,undefined);assert.equal(out.enqueuedTime,1);
out.play(100,new Float32Array(10),new Float32Array(10));
assert.equal(out.gain.gain.value,1);assert.equal(nodes[1].at,11);
console.log('BASELINE_AUDIO_STOP_LEAVES_OLD_NODE_AND_DELAY');
out.destroy();
