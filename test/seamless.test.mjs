import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {sampleCube,nearestCube,faceUV,faceNames} from '../cube.mjs';

let checks=0;
function ok(cond,msg){checks++;assert.ok(cond,msg);}
function nearVec(actual,expected,eps,ctx){
  checks++;
  assert.ok(
    actual.length===expected.length&&actual.every((a,i)=>Math.abs(a-expected[i])<=eps),
    `${ctx}: expected [${expected}], got [${actual}]`);
}
function expectRange(fn,label){
  checks++;
  assert.throws(fn,RangeError,label);
}

// Rebuild the color purely from the tap list: the trace must explain the result.
function colorFromTaps(cube,taps){
  const c=[0,0,0];
  for(const t of taps){
    const mip=cube.levels[t.level],n=mip.size,d=mip.faces[t.face],o=3*(t.y*n+t.x);
    c[0]+=t.weight*d[o];c[1]+=t.weight*d[o+1];c[2]+=t.weight*d[o+2];
  }
  return c;
}
function checkTrace(cube,dir,lod,result){
  const ctx=`dir=${JSON.stringify(dir)} lod=${lod}`;
  nearVec(colorFromTaps(cube,result.taps),result.color,1e-6,`tap list must reconstruct color (${ctx})`);
  const wsum=result.taps.reduce((a,t)=>a+t.weight,0);
  ok(Math.abs(wsum-1)<=1e-6,`tap weights must sum to 1, got ${wsum} (${ctx})`);
}

// Spec's inverse face mapping, used only to aim directions at exact (face,u,v) spots.
const invDir={
  px:(s,t)=>[1,-t,-s], nx:(s,t)=>[-1,-t,s],
  py:(s,t)=>[s,1,t],  ny:(s,t)=>[s,-1,-t],
  pz:(s,t)=>[s,-t,1], nz:(s,t)=>[-s,-t,-1],
};
const dirOnFace=(face,u,v)=>invDir[face](2*u-1,2*v-1);

const constFaces=(n,rgb)=>Object.fromEntries(
  faceNames.map(f=>[f,Array.from({length:3*n*n},(_,i)=>rgb[i%3])]));

function main(){
  // ---- 1. axis fixture: cross-face bilinear vs old nearest ----
  const axis=JSON.parse(readFileSync(new URL('../fixtures/axis.json',import.meta.url),'utf8'));
  {
    const d=[1,0,1],r=sampleCube(axis,d,0);
    nearVec(r.color,[0.5,0,0.5],1e-6,`axis seam dir=${d}`);
    ok(r.taps.length===4,`integer lod must give 4 taps, got ${r.taps.length} (dir=${d})`);
    ok(r.taps[0].face==='pz'&&r.taps[1].face==='px',
      `taps must name the faces actually read (dir=${d}): ${JSON.stringify(r.taps)}`);
    ok(r.taps[2].weight===0&&r.taps[3].weight===0,
      `zero-weight taps must be kept, not dropped (dir=${d})`);
    nearVec(nearestCube(axis,d),[1,0,0],0,'nearestCube regression (same-face nearest)');
    checkTrace(axis,d,0,r);
  }
  {
    const d=[1,1,1],r=sampleCube(axis,d,0);
    nearVec(r.color,[0.25,0.5,0.25],1e-6,`axis corner tie dir=${d}`);
    const seq=r.taps.map(t=>t.face).join(',');
    ok(seq==='py,py,pz,px',`corner tap faces py,py,pz,px, got ${seq} (dir=${d})`);
    ok(r.taps[0].x===0&&r.taps[0].y===0&&r.taps[1].x===0&&r.taps[1].y===0
       &&r.taps[0].face===r.taps[1].face,
      `duplicate pixel reads must stay separate taps (dir=${d})`);
    ok(r.taps.every(t=>Math.abs(t.weight-0.25)<=1e-12),`corner weights all 0.25 (dir=${d})`);
    checkTrace(axis,d,0,r);
  }

  // ---- 2. 2x2 unique pixels: all 12 edges, both signs, in-face rotation ----
  const edgeCube={levels:[
    {size:2,faces:Object.fromEntries(faceNames.map((f,fi)=>{
      const d=[];
      for(let j=0;j<2;j++)for(let i=0;i<2;i++){const id=fi*4+j*2+i+1;d.push(id,0.5,-id);}
      return [f,d];
    }))},
    {size:1,faces:constFaces(1,[0,0,0])},
  ]};
  // Expected neighbor (face,x,y) for the two out-of-face taps of each directed edge,
  // derived from the spec's inverse maps with tap centers s/t = +/-1.5, +/-0.5.
  const edgeTable={
    px:{left:[['pz',1,0],['pz',1,1]],right:[['nz',0,0],['nz',0,1]],
        top:[['py',1,1],['py',1,0]],bottom:[['ny',1,0],['ny',1,1]]},
    nx:{left:[['nz',1,0],['nz',1,1]],right:[['pz',0,0],['pz',0,1]],
        top:[['py',0,0],['py',0,1]],bottom:[['ny',0,1],['ny',0,0]]},
    py:{left:[['nx',0,0],['nx',1,0]],right:[['px',1,0],['px',0,0]],
        top:[['nz',1,0],['nz',0,0]],bottom:[['pz',0,0],['pz',1,0]]},
    ny:{left:[['nx',1,1],['nx',0,1]],right:[['px',0,1],['px',1,1]],
        top:[['pz',0,1],['pz',1,1]],bottom:[['nz',1,1],['nz',0,1]]},
    pz:{left:[['nx',1,0],['nx',1,1]],right:[['px',0,0],['px',0,1]],
        top:[['py',0,1],['py',1,1]],bottom:[['ny',0,0],['ny',1,0]]},
    nz:{left:[['px',1,0],['px',1,1]],right:[['nx',0,0],['nx',0,1]],
        top:[['py',1,0],['py',0,0]],bottom:[['ny',1,1],['ny',0,1]]},
  };
  const edgeUV={left:[0.125,0.5],right:[0.875,0.5],top:[0.5,0.125],bottom:[0.5,0.875]};
  const edgeTapIdx={left:[0,2],right:[1,3],top:[0,1],bottom:[2,3]};
  const edgeInIdx={left:[1,3],right:[0,2],top:[2,3],bottom:[0,1]};
  for(const face of faceNames){
    for(const edge of ['left','right','top','bottom']){
      const [u,v]=edgeUV[edge],dir=dirOnFace(face,u,v);
      const r=sampleCube(edgeCube,dir,0);
      const ctx=`face=${face} edge=${edge} dir=${JSON.stringify(dir)} level=0`;
      ok(r.taps.length===4,`4 taps expected (${ctx})`);
      edgeTapIdx[edge].forEach((ti,k)=>{
        const t=r.taps[ti],[ef,ex,ey]=edgeTable[face][edge][k];
        ok(t.face===ef&&t.x===ex&&t.y===ey,
          `cross-face tap ${ti} must land on ${ef}(${ex},${ey}), got ${t.face}(${t.x},${t.y}) (${ctx})`);
      });
      for(const ti of edgeInIdx[edge])
        ok(r.taps[ti].face===face,
          `in-face tap ${ti} must stay on ${face}, got ${r.taps[ti].face} (${ctx})`);
      checkTrace(edgeCube,dir,0,r);
    }
  }

  // ---- 3. lod blending, clamping, last level ----
  const lodCube={levels:[
    {size:2,faces:constFaces(2,[2,0,0])},
    {size:1,faces:constFaces(1,[0,0,4])},
  ]};
  {
    const d=[1,0.25,0.75];
    const r=sampleCube(lodCube,d,0.5);
    nearVec(r.color,[1,0,2],1e-6,`lod=0.5 blend dir=${JSON.stringify(d)}`);
    ok(r.taps.length===8,`fractional lod must give 8 taps, got ${r.taps.length}`);
    ok(r.taps.slice(0,4).every(t=>t.level===0)&&r.taps.slice(4).every(t=>t.level===1),
      `taps ordered low level then high level: ${JSON.stringify(r.taps.map(t=>t.level))}`);
    const sumW=ts=>ts.reduce((a,t)=>a+t.weight,0);
    ok(Math.abs(sumW(r.taps.slice(0,4))-0.5)<=1e-12&&Math.abs(sumW(r.taps.slice(4))-0.5)<=1e-12,
      `lod=0.5 per-level weight sums must be 0.5`);
    checkTrace(lodCube,d,0.5,r);

    const q=sampleCube(lodCube,d,0.25);
    nearVec(q.color,[1.5,0,1],1e-6,'lod=0.25 blend');
    checkTrace(lodCube,d,0.25,q);

    const lo=sampleCube(lodCube,d,-3);
    nearVec(lo.color,[2,0,0],1e-6,'lod below range clamps to 0');
    ok(lo.taps.length===4&&lo.taps.every(t=>t.level===0),'clamped low lod uses level 0 only');

    const hi=sampleCube(lodCube,d,99);
    nearVec(hi.color,[0,0,4],1e-6,'lod above range clamps to last level');
    ok(hi.taps.length===4&&hi.taps.every(t=>t.level===1),'clamped high lod uses last level only');

    const last=sampleCube(lodCube,d,1);
    nearVec(last.color,[0,0,4],1e-6,'integer lod at last level');
    ok(last.taps.length===4,'integer lod uses a single level (4 taps)');
  }

  // ---- 4. odd-size 3 -> 1 chain ----
  const oddCube={levels:[
    {size:3,faces:Object.fromEntries(faceNames.map(f=>{
      const d=new Array(27).fill(0);
      if(f==='px'){d[3*(1*3+1)]=9;d[3*(1*3+1)+1]=9;d[3*(1*3+1)+2]=9;}
      if(f==='pz'){d[3*(1*3+2)+1]=7;}
      return [f,d];
    }))},
    {size:1,faces:constFaces(1,[0,0,0])},
  ]};
  {
    const d=[1,0,0],r=sampleCube(oddCube,d,0);
    nearVec(r.color,[9,9,9],1e-6,`odd chain center texel dir=${d}`);
    checkTrace(oddCube,d,0,r);

    const d2=[1,0,0.8],r2=sampleCube(oddCube,d2,0); // u=0.1,v=0.5 on px: left-edge crossing
    nearVec(r2.color,[0,1.4,0],1e-6,`odd chain cross-face dir=${d2}`);
    const t0=r2.taps[0];
    ok(t0.face==='pz'&&t0.x===2&&t0.y===1&&Math.abs(t0.weight-0.2)<=1e-12,
      `odd chain cross tap must be pz(2,1) w=0.2, got ${JSON.stringify(t0)} (dir=${d2})`);
    checkTrace(oddCube,d2,0,r2);
  }

  // ---- 5. direction scale invariance ----
  {
    const a=sampleCube(lodCube,[1,0.25,0.75],0.3);
    const b=sampleCube(lodCube,[7,1.75,5.25],0.3);
    nearVec(b.color,a.color,1e-12,'direction scaled by 7: same color');
    checks++;
    assert.deepEqual(b.taps,a.taps,'direction scaled by 7: same taps');
  }

  // ---- 6. invalid textures and parameters -> RangeError ----
  const validFaces=n=>constFaces(n,[0,0,0]);
  expectRange(()=>sampleCube({levels:[]},[1,0,0],0),'empty levels');
  expectRange(()=>sampleCube({},[1,0,0],0),'missing levels');
  expectRange(()=>sampleCube({levels:[{size:2,faces:validFaces(2)}]},[1,0,0],0),'last level not size 1');
  expectRange(()=>sampleCube({levels:[{size:0,faces:validFaces(1)}]},[1,0,0],0),'size 0');
  expectRange(()=>sampleCube({levels:[{size:1.5,faces:validFaces(1)}]},[1,0,0],0),'non-integer size');
  expectRange(()=>sampleCube({levels:[{size:4,faces:validFaces(4)},{size:1,faces:validFaces(1)}]},[1,0,0],0),'chain 4->1 skips 2');
  expectRange(()=>sampleCube({levels:[{size:2,faces:validFaces(2)},{size:2,faces:validFaces(2)}]},[1,0,0],0),'chain 2->2 not halved');
  expectRange(()=>sampleCube({levels:[{size:3,faces:validFaces(3)},{size:2,faces:validFaces(2)}]},[1,0,0],0),'chain 3->2 wrong floor');
  expectRange(()=>sampleCube({levels:[{size:2,faces:validFaces(2)},{size:1,faces:validFaces(1)},{size:1,faces:validFaces(1)}]},[1,0,0],0),'duplicate level after size 1');
  {
    const bad={size:1,faces:validFaces(1)};delete bad.faces.nz;
    expectRange(()=>sampleCube({levels:[bad]},[1,0,0],0),'missing face nz');
    const short={size:1,faces:validFaces(1)};short.faces.px=[1,0];
    expectRange(()=>sampleCube({levels:[short]},[1,0,0],0),'face data wrong length');
    const nan={size:1,faces:validFaces(1)};nan.faces.py=[0,NaN,0];
    expectRange(()=>sampleCube({levels:[nan]},[1,0,0],0),'non-finite texel');
    const notArray={size:1,faces:validFaces(1)};notArray.faces.px={0:1,1:0,2:0,length:3};
    expectRange(()=>sampleCube({levels:[notArray]},[1,0,0],0),'face data not an array');
  }
  const good={levels:[{size:1,faces:validFaces(1)}]};
  expectRange(()=>sampleCube(good,[0,0,0],0),'zero direction');
  expectRange(()=>sampleCube(good,[NaN,1,0],0),'NaN direction');
  expectRange(()=>sampleCube(good,[Infinity,1,0],0),'infinite direction');
  expectRange(()=>sampleCube(good,[1,0,0],NaN),'NaN lod');
  expectRange(()=>sampleCube(good,[1,0,0],Infinity),'infinite lod');
  expectRange(()=>sampleCube(good,[1,0,0],-Infinity),'-infinite lod');

  // ---- 7. inputs must not be modified (also covers typed-array faces) ----
  {
    const typed={levels:[
      {size:2,faces:Object.fromEntries(faceNames.map((f,fi)=>{
        const d=new Float32Array(12);
        for(let k=0;k<12;k++)d[k]=fi*12+k+0.25;
        return [f,d];
      }))},
      {size:1,faces:Object.fromEntries(faceNames.map((f,fi)=>[f,new Float32Array([fi,0.5,1])]))},
    ]};
    const dir=[1,0.25,0.75];
    const cubeSnap=structuredClone(typed),dirSnap=dir.slice();
    const r=sampleCube(typed,dir,0.7);
    ok(r.taps.length===8,'typed-array cube samples fine');
    checkTrace(typed,dir,0.7,r);
    checks++;
    assert.deepEqual(typed,cubeSnap,'cube texture must be unmodified');
    checks++;
    assert.deepEqual(dir,dirSnap,'direction must be unmodified');
  }
}

try{
  main();
}catch(err){
  console.error(`SEAMLESS_FAIL after ${checks} checks: ${err.message}`);
  process.exit(1);
}
console.log(`SEAMLESS_OK: ${checks} checks (axis seam/corner, 24 directed edges, lod blend/clamp, odd chain, scale, validation, immutability)`);
