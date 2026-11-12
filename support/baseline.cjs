const assert=require('node:assert/strict');
const j=require(process.env.JSFEAT_MODULE);
const w=128,h=128;
function texture(x,y) {return 110+28*Math.sin(x*.22)+24*Math.cos(y*.19)+18*Math.sin(x*.13+y*.17);}
function image(dx,dy,gain=1,bias=0) {
 const m=new j.matrix_t(w,h,j.U8C1_t);
 for(let y=0;y<h;y++)for(let x=0;x<w;x++)m.data[y*w+x]=Math.round(gain*texture(x-dx,y-dy)+bias);
 return m;
}
function pyramid(m){const p=new j.pyramid_t(3);p.allocate(w,h,j.U8C1_t);p.build(m,false);return p;}
const first=pyramid(image(0,0));
function flow(g,b) {
 const output=new Float32Array(2),status=new Uint8Array(1);
 j.optical_flow_lk.track(first,pyramid(image(2,-1,g,b)),new Float32Array([64,64]),output,1,15,40,status,0.001,0.0001);
 assert.equal(status[0],1);return Math.hypot(output[0]-66,output[1]-63);
}
const same=flow(1,0),lit=flow(1.35,-18);
assert(same<0.1);assert(lit>0.3);
console.log(`BASELINE LK-brightness PASS ordinary=${same.toFixed(6)} relit=${lit.toFixed(6)}`);
