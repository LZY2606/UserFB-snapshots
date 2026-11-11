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
