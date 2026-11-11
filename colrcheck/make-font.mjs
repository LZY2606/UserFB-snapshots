// SPDX-License-Identifier: MIT
// Original geometric test font, generated without any third-party font assets.
import {writeFileSync,readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
const U16=n=>{let b=Buffer.alloc(2);b.writeUInt16BE(n&65535);return b;};
const I16=U16;
const U32=n=>{let b=Buffer.alloc(4);b.writeUInt32BE(n>>>0);return b;};
const I32=n=>{let b=Buffer.alloc(4);b.writeInt32BE(n);return b;};
const U24=n=>Buffer.from([(n>>>16)&255,(n>>>8)&255,n&255]);
const cat=(...b)=>Buffer.concat(b.flat());
const pad=b=>cat(b,Buffer.alloc((4-b.length%4)%4));
function rect(x0,y0,x1,y1){
 const pts=[[x0,y0],[x0,y1],[x1,y1],[x1,y0]];
 return cat(I16(1),[x0,y0,x1,y1].map(I16),U16(3),U16(0),Buffer.from([1,1,1,1]),
 pts.map((p,i)=>I16(p[0]-(i?pts[i-1][0]:0))),pts.map((p,i)=>I16(p[1]-(i?pts[i-1][1]:0))));
}
let glyphs=[rect(0,0,100,100),Buffer.alloc(0),rect(0,0,800,800),rect(200,200,600,600),rect(0,0,400,800),Buffer.alloc(0),Buffer.alloc(0),Buffer.alloc(0)];
let locations=[0],g=[];for(let x of glyphs){x=pad(x);g.push(x);locations.push(locations.at(-1)+x.length);}
let head=Buffer.alloc(54);head.writeUInt32BE(0x10000,0);head.writeUInt32BE(0x5f0f3cf5,12);head.writeUInt16BE(1000,18);head.writeBigUInt64BE(3782844800n,20);head.writeBigUInt64BE(3782844800n,28);head.writeInt16BE(800,40);head.writeInt16BE(800,42);head.writeInt16BE(1,50);
let hhea=Buffer.alloc(36);hhea.writeUInt32BE(0x10000);hhea.writeInt16BE(800,4);hhea.writeInt16BE(-200,6);hhea.writeUInt16BE(900,10);hhea.writeUInt16BE(glyphs.length,34);
let maxp=Buffer.alloc(32);maxp.writeUInt32BE(0x10000);maxp.writeUInt16BE(glyphs.length,4);maxp.writeUInt16BE(4,6);maxp.writeUInt16BE(1,8);maxp.writeUInt16BE(2,14);
let groups=[[64,64,2],[65,65,1],[100,100,2],[117,117,2]];
let cmap12=cat(U16(12),U16(0),U32(16+12*groups.length),U32(0),U32(groups.length),groups.map(a=>cat(a.map(U32))));
let cmap=cat(U16(0),U16(1),U16(3),U16(10),U32(12),cmap12);
// CPAL v0: two palettes. Entries are BGRA in the font, not RGBA.
let colors=[[0,0,255,255],[255,0,0,255],[0,255,0,128],[0,255,0,255],[0,0,255,128],[255,255,0,255]];
let cpal=cat(U16(0),U16(3),U16(2),U16(6),U32(16),U16(0),U16(3),...colors.map(a=>Buffer.from(a)));
// Build the actual COLRv1 tables. Offsets use each OpenType record's own base.
let nodes=[],fixups=[];
function node(name,bytes){nodes.push({name,bytes});return name;}
function link(from,at,to,kind='paint'){fixups.push({from,at,to,kind});}
function solid(name,index,alpha=1){return node(name,cat(Buffer.from([2]),U16(index),I16(Math.round(alpha*16384))));}
function glyph(name,id,child){node(name,cat(Buffer.from([10]),U24(0),U16(id)));link(name,1,child);return name;}
function ref(name,id){return node(name,cat(Buffer.from([11]),U16(id)));}
function transform(name,child,m){node(name,cat(Buffer.from([12]),U24(0),U24(0)));link(name,1,child);node(name+'matrix',cat(m.map(v=>I32(Math.round(v*65536)))));link(name,4,name+'matrix');return name;}
node('root1',cat(Buffer.from([1,2]),U32(0)));
node('root6',cat(Buffer.from([1,2]),U32(2)));
node('root7',cat(Buffer.from([32]),U24(0),Buffer.from([5]),U24(0))); // SRC_IN
link('root7',1,'source7');link('root7',5,'backdrop7');
transform('left','reuseLeft',[0.5,0,0,1,0,0]);ref('reuseLeft',5);
transform('right','reuseRight',[-0.5,0,0,1,800,0]);ref('reuseRight',5);
glyph('red',2,'redFill');glyph('blue',3,'blueFill');
glyph('shared5',2,'gradient');glyph('source7',3,'gradient');glyph('backdrop7',2,'halfRed');
solid('redFill',0);solid('blueFill',1,0.5);solid('halfRed',0,0.5);
node('gradient',cat(Buffer.from([4]),U24(0),[0,0,800,0,400,800].map(I16)));link('gradient',1,'colorline');
node('colorline',cat(Buffer.from([0]),U16(2),I16(0),U16(0),I16(16384),I16(16384),U16(2),I16(16384)));
let bases=[[1,'root1'],[5,'shared5'],[6,'root6'],[7,'root7']];let layers=['red','blue','left','right'];
const baseOff=34,layerOff=baseOff+4+6*bases.length;
let pos=layerOff+4+4*layers.length;let byName=new Map;
for(const n of nodes){n.pos=pos;pos+=n.bytes.length;byName.set(n.name,n);}
for(const f of fixups){let a=byName.get(f.from),b=byName.get(f.to);let delta=b.pos-a.pos;if(delta<0)throw Error('non-forward paint offset');U24(delta).copy(a.bytes,f.at);}
let colr=cat(U16(1),U16(0),U32(0),U32(0),U16(0),U32(baseOff),U32(layerOff),U32(0),U32(0),U32(0),
 U32(bases.length),bases.map(([id,name])=>cat(U16(id),U32(byName.get(name).pos-baseOff))),
 U32(layers.length),layers.map(name=>U32(byName.get(name).pos-layerOff)),nodes.map(n=>n.bytes));
let nameString=Buffer.from('00580031003000350034002000470065006f006d0065007400720079','hex');
let name=cat(U16(0),U16(1),U16(18),U16(3),U16(1),U16(0x409),U16(1),U16(nameString.length),U16(0),nameString);
let post=Buffer.alloc(32);post.writeUInt32BE(0x30000);
let tables={COLR:colr,CPAL:cpal,cmap,glyf:cat(g),head,hhea,hmtx:cat(glyphs.map(()=>cat(U16(900),I16(0)))),loca:cat(locations.map(U32)),maxp,name,post};
function checksum(b){b=pad(b);let s=0;for(let i=0;i<b.length;i+=4)s=(s+b.readUInt32BE(i))>>>0;return s;}
let tags=Object.keys(tables).sort(),n=tags.length,power=2**Math.floor(Math.log2(n)),offset=12+16*n,records=[],body=[],manifest={};
for(let tag of tags){let b=tables[tag];manifest[tag]={offset,length:b.length};records.push(cat(Buffer.from(tag),U32(checksum(b)),U32(offset),U32(b.length)));body.push(pad(b));offset+=pad(b).length;}
let font=cat(U32(0x10000),U16(n),U16(power*16),U16(Math.log2(power)),U16(n*16-power*16),records,body);
font.writeUInt32BE((0xb1b0afba-checksum(font))>>>0,manifest.head.offset+8);
const target=new URL('./geometry.ttf',import.meta.url),sha=createHash('sha256').update(font).digest('hex');
if(process.argv.includes('--check')){if(!readFileSync(target).equals(font))throw Error('generated font differs');console.log('FONT_REPRO_OK sha256='+sha);}
else{writeFileSync(target,font);writeFileSync(new URL('./font-manifest.json',import.meta.url),JSON.stringify({sha256:sha,bytes:font.length,tables:manifest,baseGlyphs:bases.map(x=>x[0]),nodes:nodes.map(x=>({name:x.name,offset:x.pos,bytes:x.bytes.length}))},null,2)+'\n');console.log('FONT_GENERATED bytes='+font.length+' sha256='+sha);}
