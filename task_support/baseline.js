"use strict";
const assert=require('node:assert/strict');
const api=require('../index.js');
const m=api.surfaceNets([4,5,6],(x,y,z)=>x-12.5,[[10,20,30],[18,35,54]]);
assert.equal(m.positions.length,20); assert.equal(m.cells.length,24);
for(const p of m.positions){ assert.ok(Math.abs(p[0]-12.5)<1e-12); assert.ok(p.every(Number.isFinite)); }
for(const t of m.cells){
 assert.equal(t.length,3); for(const i of t) assert.ok(Number.isInteger(i)&&i>=0&&i<m.positions.length);
 const [a,b,c]=t.map(i=>m.positions[i]);
 const cross=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]); assert.ok(cross>0);
}
console.log('BASELINE_PLANE vertices=20 triangles=24 winding=positive anisotropic=ok');
const s=api.surfaceNets([9,9,9],(x,y,z)=>(x-4)**2+(y-4)**2+(z-4)**2-6.25);
assert.ok(s.positions.length>0&&s.cells.length>0);
const edges=new Map();
for(const t of s.cells){ for(let j=0;j<3;j++){ const a=t[j],b=t[(j+1)%3]; assert.ok(a!==b&&a>=0&&b>=0&&a<s.positions.length&&b<s.positions.length); const k=[a,b].sort((a,b)=>a-b).join(','); edges.set(k,(edges.get(k)||0)+1); } }
for(const n of edges.values())assert.equal(n,2);
assert.equal(s.positions.length-edges.size+s.cells.length,2);
for(const v of s.positions)assert.ok(v.every(Number.isFinite));
console.log(`BASELINE_SPHERE vertices=${s.positions.length} triangles=${s.cells.length} closed=ok euler=2`);
const empty=api.surfaceNets([3,4,5],()=>1); assert.equal(empty.positions.length,0); assert.equal(empty.cells.length,0);
assert.deepEqual(api.surfaceNets([4,5,6],x=>x-12.5,[[10,20,30],[18,35,54]]),m);
console.log('BASELINE_REUSE empty=ok repeated=equal');
