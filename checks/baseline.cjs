const fs=require('node:fs'),assert=require('node:assert/strict'),Typr=require('./load.cjs');
const b=fs.readFileSync('demo/LiberationSans-Bold.ttf');
const f=Typr.parse(b.buffer.slice(b.byteOffset,b.byteOffset+b.byteLength))[0];
const s=Typr.U.shape(f,'AV');
assert.equal(s.length,2); assert(s.every(g=>g.g>0&&g.ax>0));
const p=Typr.U.shapeToPath(f,s); assert(p.cmds.length>0&&p.crds.every(Number.isFinite));
console.log('BASE_TYPR_AV glyphs='+s.map(g=>g.g).join(',')+' advances='+s.map(g=>g.ax).join(',')+' path=PASS');
