import assert from 'node:assert/strict';
import VideoContext from '../src/videocontext.js';
globalThis.window = {};
globalThis.HTMLMediaElement = {HAVE_FUTURE_DATA:3};
const gl = new Proxy({canvas:{width:320,height:180}}, {get(o,k) {
 if (k in o) return o[k];
 if (/^[A-Z_0-9]+$/.test(k)) return 1;
 if (k==='getParameter') return ()=>8;
 if (k==='getShaderParameter'||k==='getProgramParameter') return ()=>true;
 if (k.startsWith('create')) return ()=>({});
 return ()=>{};
}});
const element = ()=>({duration:10,readyState:4,seeking:false,ended:false,
 currentTime:0,playbackRate:1,pause(){},play(){return Promise.resolve();}});
const ctx = new VideoContext({id:'baseline',getContext:()=>gl},null,
 {manualUpdate:true,useVideoElementCache:false});
const ve=element(), ae=element();
const v=ctx.video(ve,1), a=ctx.audio(ae,1);
for(const n of [v,a]) {assert(n.startAt(10));n.playbackRate=2;n._seek(12);n._update(12);}
assert.equal(ve.currentTime,3);assert.equal(ae.currentTime,3);
assert.equal(v.stopTime,20);assert.equal(a.stopTime,20);
ctx.playbackRate=3;
v._update(12);a._update(12);
assert.equal(ve.playbackRate,6);assert.equal(ae.playbackRate,2);
console.log('SEED_TIMELINE_OK seek=3 inferred_stop=20 video_rate=6 audio_rate=2');
