export const faceNames = ['px','nx','py','ny','pz','nz'];
export function faceUV([x,y,z]) {
  const ax=Math.abs(x), ay=Math.abs(y), az=Math.abs(z);
  if (![x,y,z].every(Number.isFinite) || Math.max(ax,ay,az)===0)
    throw new RangeError('invalid direction');
  let face,s,t,m;
  if(ax>=ay && ax>=az){m=ax;if(x>=0){face='px';s=-z;t=-y;}else{face='nx';s=z;t=-y;}}
  else if(ay>=az){m=ay;if(y>=0){face='py';s=x;t=z;}else{face='ny';s=x;t=-z;}}
  else {m=az;if(z>=0){face='pz';s=x;t=-y;}else{face='nz';s=-x;t=-y;}}
  return {face,u:(s/m+1)/2,v:(t/m+1)/2};
}
export function nearestCube(cube,direction,level=0) {
  const {face,u,v}=faceUV(direction),mip=cube.levels[level],n=mip.size;
  const x=Math.max(0,Math.min(n-1,Math.floor(u*n)));
  const y=Math.max(0,Math.min(n-1,Math.floor(v*n)));
  const i=3*(y*n+x);
  return Array.from(mip.faces[face].slice(i,i+3));
}

// Inverse of the per-face (s,t) mapping, back to a direction vector.
const faceInvDir={
  px:(s,t)=>[1,-t,-s],
  nx:(s,t)=>[-1,-t,s],
  py:(s,t)=>[s,1,t],
  ny:(s,t)=>[s,-1,-t],
  pz:(s,t)=>[s,-t,1],
  nz:(s,t)=>[-s,-t,-1],
};

function validateCube(cube){
  if(!cube||typeof cube!=='object'||!Array.isArray(cube.levels)||cube.levels.length<1)
    throw new RangeError('cube.levels must be a non-empty array');
  const levels=cube.levels;
  for(let li=0;li<levels.length;li++){
    const mip=levels[li];
    if(!mip||typeof mip!=='object'||!Number.isInteger(mip.size)||mip.size<1)
      throw new RangeError(`level ${li}: size must be a positive integer`);
    const n=mip.size;
    if(li>0){
      const prev=levels[li-1].size;
      if(prev<2||n!==Math.floor(prev/2))
        throw new RangeError(`level ${li}: size ${n} does not follow size ${prev}`);
    }
    if(li===levels.length-1&&n!==1)
      throw new RangeError(`level ${li}: last level must have size 1, got ${n}`);
    if(!mip.faces||typeof mip.faces!=='object')
      throw new RangeError(`level ${li}: faces must be an object`);
    for(const f of faceNames){
      const data=mip.faces[f];
      if(!(Array.isArray(data)||ArrayBuffer.isView(data))||data.length!==3*n*n)
        throw new RangeError(`level ${li} face ${f}: expected exactly ${3*n*n} values`);
      for(let k=0;k<data.length;k++)
        if(!Number.isFinite(data[k]))
          throw new RangeError(`level ${li} face ${f}: non-finite value at index ${k}`);
    }
  }
}

function readPixel(mip,face,i,j){
  const n=mip.size,o=3*(j*n+i),d=mip.faces[face];
  return [d[o],d[o+1],d[o+2]];
}

// Bilinear sample of one mip level; taps out of the face are re-projected
// onto a neighbor face via the inverse mapping of the selected face.
function sampleLevel(cube,direction,li,levelWeight,taps){
  const mip=cube.levels[li],n=mip.size;
  const {face,u,v}=faceUV(direction);
  const x=u*n-0.5,y=v*n-0.5;
  const i0=Math.floor(x),j0=Math.floor(y);
  const fx=x-i0,fy=y-j0;
  const quad=[
    [i0,j0,(1-fx)*(1-fy)],
    [i0+1,j0,fx*(1-fy)],
    [i0,j0+1,(1-fx)*fy],
    [i0+1,j0+1,fx*fy],
  ];
  const color=[0,0,0];
  for(const [i,j,w0] of quad){
    let rf=face,ri=i,rj=j;
    if(i<0||i>=n||j<0||j>=n){
      const s=2*((i+0.5)/n)-1,t=2*((j+0.5)/n)-1;
      const r=faceUV(faceInvDir[face](s,t));
      rf=r.face;
      ri=Math.max(0,Math.min(n-1,Math.floor(r.u*n)));
      rj=Math.max(0,Math.min(n-1,Math.floor(r.v*n)));
    }
    const w=w0*levelWeight,p=readPixel(mip,rf,ri,rj);
    color[0]+=w*p[0];color[1]+=w*p[1];color[2]+=w*p[2];
    taps.push({level:li,face:rf,x:ri,y:rj,weight:w});
  }
  return color;
}

export function sampleCube(cube,direction,lod){
  validateCube(cube);
  faceUV(direction); // validates the direction (finite, non-zero)
  if(!Number.isFinite(lod)) throw new RangeError('lod must be finite');
  const L=cube.levels.length;
  const c=Math.max(0,Math.min(L-1,lod));
  const l0=Math.floor(c),frac=c-l0;
  const taps=[];
  if(frac===0) return {color:sampleLevel(cube,direction,l0,1,taps),taps};
  const a=sampleLevel(cube,direction,l0,1-frac,taps);
  const b=sampleLevel(cube,direction,l0+1,frac,taps);
  return {color:[a[0]+b[0],a[1]+b[1],a[2]+b[2]],taps};
}
