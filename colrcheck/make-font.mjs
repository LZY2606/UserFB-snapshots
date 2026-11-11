// SPDX-License-Identifier: MIT
// Original geometric test font, generated without any third-party font assets.
import {writeFileSync,readFileSync,mkdirSync} from 'node:fs';
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

// ---------------------------------------------------------------------------
// Additional contract fixtures. Standard OpenType COLR v1 / CPAL v0 layouts,
// built only from the primitives above. geometry.ttf above is never modified
// by this section.
function sfntAssemble(tables){
 function checksum(b){b=pad(b);let s=0;for(let i=0;i<b.length;i+=4)s=(s+b.readUInt32BE(i))>>>0;return s;}
 let tags=Object.keys(tables).sort(),n=tags.length,power=2**Math.floor(Math.log2(n)),off=12+16*n,records=[],body=[],man={};
 for(let tag of tags){let b=tables[tag];man[tag]={offset:off,length:b.length};records.push(cat(Buffer.from(tag),U32(checksum(b)),U32(off),U32(b.length)));body.push(pad(b));off+=pad(b).length;}
 let out=cat(U32(0x10000),U16(n),U16(power*16),U16(Math.log2(power)),U16(n*16-power*16),records,body);
 out.writeUInt32BE((0xb1b0afba-checksum(out))>>>0,man.head.offset+8);
 return {font:out,manifest:man};
}
function skeletonFont(numGlyphs,glyfGlyphs,colr,cpal,label){
 let locations=[0],gg=[];for(let x of glyfGlyphs){x=pad(x);gg.push(x);locations.push(locations.at(-1)+x.length);}
 let head=Buffer.alloc(54);head.writeUInt32BE(0x10000,0);head.writeUInt32BE(0x5f0f3cf5,12);head.writeUInt16BE(1000,18);head.writeBigUInt64BE(3782844800n,20);head.writeBigUInt64BE(3782844800n,28);head.writeInt16BE(800,40);head.writeInt16BE(800,42);head.writeInt16BE(1,50);
 let hhea=Buffer.alloc(36);hhea.writeUInt32BE(0x10000);hhea.writeInt16BE(800,4);hhea.writeInt16BE(-200,6);hhea.writeUInt16BE(900,10);hhea.writeUInt16BE(numGlyphs,34);
 let maxp=Buffer.alloc(32);maxp.writeUInt32BE(0x10000);maxp.writeUInt16BE(numGlyphs,4);maxp.writeUInt16BE(4,6);maxp.writeUInt16BE(1,8);maxp.writeUInt16BE(2,14);
 let groups=[[65,65,1]];
 let cmap12=cat(U16(12),U16(0),U32(16+12*groups.length),U32(0),U32(groups.length),groups.map(a=>cat(a.map(U32))));
 let cmap=cat(U16(0),U16(1),U16(3),U16(10),U32(12),cmap12);
 let nameBuf=Buffer.from(Buffer.from(label).toString('hex'),'hex');
 let name=cat(U16(0),U16(1),U16(18),U16(3),U16(1),U16(0x409),U16(1),U16(nameBuf.length),U16(0),nameBuf);
 let post=Buffer.alloc(32);post.writeUInt32BE(0x30000);
 return sfntAssemble({COLR:colr,CPAL:cpal,cmap,glyf:cat(gg),head,hhea,hmtx:cat(gg.map(()=>cat(U16(900),I16(0)))),loca:cat(locations.map(U32)),maxp,name,post});
}
// paint-graph builder: forward-only offsets, every offset relative to its own table record
function paintGraph(){
 const nodes=[],fixups=[];const by=new Map();
 function node(name,bytes){nodes.push({name,bytes});by.set(name,nodes.at(-1));return name;}
 function link(from,at,to){fixups.push({from,at,to});}
 return {
  solid:(name,index,alpha=1)=>node(name,cat(Buffer.from([2]),U16(index),I16(Math.round(alpha*16384)))),
  glyph:(name,id,child)=>{node(name,cat(Buffer.from([10]),U24(0),U16(id)));link(name,1,child);return name;},
  ref:(name,id)=>node(name,cat(Buffer.from([11]),U16(id))),
  transform:(name,child,m)=>{node(name,cat(Buffer.from([12]),U24(0),U24(0)));link(name,1,child);node(name+'M',cat(m.map(v=>I32(Math.round(v*65536)))));link(name,4,name+'M');return name;},
  composite:(name,mode,source,backdrop)=>{node(name,cat(Buffer.from([32]),U24(0),Buffer.from([mode]),U24(0)));link(name,1,source);link(name,5,backdrop);return name;},
  gradient:(name,pts,lineName)=>{node(name,cat(Buffer.from([4]),U24(0),pts.map(I16)));link(name,1,lineName);return name;},
  colorline:(name,stops)=>node(name,cat(Buffer.from([0]),U16(stops.length),stops.map(s=>cat(I16(Math.round(s[0]*16384)),U16(s[1]),I16(Math.round((s[2]??1)*16384)))))),
  layers:(name,count,first)=>{node(name,cat(Buffer.from([1]),Buffer.from([count]),U32(first)));return name;},
  raw:(name,bytes)=>node(name,bytes),
  build(baseGlyphs,layerEntries){
   const headerSize=34;
   const baseOff=headerSize;
   const layerOff=layerEntries.length?baseOff+4+6*baseGlyphs.length:0;
   // Kahn ordering on u24 fixup edges: a node is emitted only after every
   // node that points at it, so all paint offsets are forward
   const indeg=new Map(nodes.map(n=>[n.name,0])),edges=new Map(nodes.map(n=>[n.name,[]]));
   for(const f of fixups){edges.get(f.from).push(f.to);indeg.set(f.to,indeg.get(f.to)+1);}
   const queue=nodes.filter(n=>indeg.get(n.name)===0).map(n=>n.name),order=[];
   while(queue.length){
    const nm=queue.shift();order.push(nm);
    for(const d of edges.get(nm)){indeg.set(d,indeg.get(d)-1);if(indeg.get(d)===0)queue.push(d);}
   }
   if(order.length!==nodes.length)throw Error('paint offset cycle in fixture');
   const afterBase=baseOff+4+6*baseGlyphs.length;
   let pos=layerEntries.length?layerOff+4+4*layerEntries.length:afterBase;
   for(const nm of order){const n=by.get(nm);n.pos=pos;pos+=n.bytes.length;}
   for(const f of fixups){const a=by.get(f.from),b=by.get(f.to);const d=b.pos-a.pos;if(d<0)throw Error('non-forward paint offset '+f.from+' -> '+f.to+' d='+d);U24(d).copy(a.bytes,f.at);}
   const layerList=layerEntries.length?cat(U32(layerEntries.length),layerEntries.map(nm=>U32(by.get(nm).pos-layerOff))):Buffer.alloc(0);
   const colr=cat(
    U16(1),U16(0),U32(0),U32(0),U16(0),
    U32(baseOff),U32(layerOff),U32(0),U32(0),U32(0),
    U32(baseGlyphs.length),baseGlyphs.map(([id,nm])=>cat(U16(id),U32(by.get(nm).pos-baseOff))),
    layerList,order.map(nm=>by.get(nm).bytes));
   return {colr,nodes:order.map(nm=>by.get(nm))};
  }
 };
}
function cpalTable(entriesPerPalette,paletteBases,bgra){
 const colorRecordsOff=12+2*paletteBases.length;
 return cat(U16(0),U16(entriesPerPalette),U16(paletteBases.length),U16(bgra.length),U32(colorRecordsOff),
  paletteBases.map(U16),bgra.map(a=>Buffer.from(a)));
}
// CPAL pools (BGRA bytes). Three palettes over nine records; palette 2 reuses palette 0.
const CP0=[[0,0,255,255],[255,0,0,128],[0,255,0,255]];             // red, half blue, green
const CP1=[[255,255,0,255],[128,0,128,128],[255,255,255,255]];     // cyan, half purple, white
const contractCpal=cpalTable(3,[0,3,6],[...CP0,...CP1,...CP0]);
{
 // glyf: g0 empty, g1 empty, g2 unit square, g3 inset square, g4 half square, g5..g12 empty COLR bases
 const empty=Buffer.alloc(0);
 const glyf=[empty,empty,rect(0,0,100,100),rect(20,20,60,60),rect(0,0,40,80),empty,empty,empty,empty,empty,empty,empty,empty];
 const P=paintGraph();
 const line1=P.colorline('line1',[[0,0,1],[1,2,1]]);                       // red -> green
 const line2=P.colorline('line2',[[0,0,1],[1,0xffff,1]]);                  // red -> caller foreground
 const pts=[0,0,100,0,50,100];                                             // non-orthogonal three-point line
 P.gradient('grad1',pts,line1);P.gradient('grad2',pts,line2);
 // index 1 is half-alpha blue in palette 0 and half-alpha purple in palette 1
 P.solid('red',0);P.solid('blueHalf',1);P.solid('green',2);P.solid('purpleHalf',1);
 P.glyph('g2red',2,'red');P.glyph('g3blue',3,'blueHalf');P.glyph('g4green',4,'green');
 P.glyph('g3grad',3,'grad1');P.glyph('g2grad',2,'grad1');P.glyph('g2grad2',2,'grad2');
 P.glyph('g2purple',2,'purpleHalf');
 // g1: two stacked solid layers (LayerList entries 0..1)
 P.layers('root1',2,0);
 // g5: transform outside PaintGlyph moves outline and color field together
 P.transform('t5','g2grad',[0.8,0,0,0.8,10.5,7.25]);
 // g6: transform wraps only the gradient fill (outline stays put)
 P.transform('t6','grad1',[1,0,0,1,12,-8]);P.glyph('root6',2,'t6');
 // g8: shared subgraph with a foreground-color stop
 const shared='g2grad2';
 // g7: same shared graph reached through two genuinely non-commuting transforms
 P.transform('a7',shared,[1,0.25,0,1,0,0]);          // y-shear
 P.transform('b7',shared,[0.75,0,0.3,0.75,20,10]);   // scale plus x-shear
 P.transform('ab7','a7',[0.75,0,0.3,0.75,20,10]);
 P.transform('ba7','b7',[1,0.25,0,1,0,0]);
 P.layers('root7',2,2); // entries 2..3
 // g9: outer sibling plus isolated SRC_IN (sibling must not act as backdrop)
 P.composite('cin9',5,'g3grad','g2purple');
 P.layers('root9',2,4); // entries 4..5
 // g10: translucent SRC_OVER
 P.composite('root10',3,'g3blue','g2purple');
 // g11: clip restore - inner g3 clip ends before g4 paints
 P.layers('root11',2,6); // entries 6..7
 // g12: fractional translation proves no integer snapping of the outline
 P.transform('root12','g2red',[1,0,0,1,3.5,2.25]);
 const bases=[[1,'root1'],[5,'t5'],[6,'root6'],[7,'root7'],[8,shared],[9,'root9'],[10,'root10'],[11,'root11'],[12,'root12']];
 const layerNames=['g2red','g3blue','ab7','ba7','g4green','cin9','g3blue','g4green'];
 const {colr,nodes}=P.build(bases,layerNames);
 const built=skeletonFont(glyf.length,glyf,colr,contractCpal,'X1054 Contract');
 const sha=createHash('sha256').update(built.font).digest('hex');
 const dir=new URL('./fixtures/',import.meta.url);
 mkdirSync(dir,{recursive:true});
 writeFileSync(new URL('contract.ttf',dir),built.font);
 writeFileSync(new URL('fixtures-manifest.json',dir),JSON.stringify({contract:{sha256:sha,bytes:built.font.length,tables:built.manifest,
  baseGlyphs:bases.map(x=>x[0]),palettes:3,paletteEntries:3,paletteBases:[0,3,6],
  colors:{red:[0,0,255,255],blueHalf:[255,0,0,128],green:[0,255,0,255],cyan:[255,255,0,255],purpleHalf:[128,0,128,128],white:[255,255,255,255]},
  gradientPoints:{p0:[0,0],p1:[100,0],p2:[50,100]},nodes:nodes.map(n=>({name:n.name,offset:n.pos,bytes:n.bytes.length}))}},null,2)+'\n');
}
{
 // errors.ttf: one good base plus cycle / missing ref / unsupported format / unsupported mode
 const glyf=[Buffer.alloc(0),rect(0,0,100,100),Buffer.alloc(0),Buffer.alloc(0),Buffer.alloc(0),Buffer.alloc(0),Buffer.alloc(0)];
 const P=paintGraph();
 P.solid('red',0);P.glyph('good',1,'red');
 P.ref('selfCycle',2);                       // base g2 referencing itself
 P.ref('missingRef',900);                    // glyph index outside the font
 P.raw('badFormat',cat(Buffer.from([6]),U24(0),U16(1))); // PaintSkew-ish, unsupported
 P.composite('badMode',6,'good','good');    // unsupported composite mode
 const bases=[[2,'selfCycle'],[3,'missingRef'],[4,'badFormat'],[5,'badMode'],[6,'good']];
 const {colr}=P.build(bases,[]);
 const built=skeletonFont(glyf.length,glyf,colr,contractCpal,'X1054 Errors');
 const sha=createHash('sha256').update(built.font).digest('hex');
 const dir=new URL('./fixtures/',import.meta.url);
 const existing=JSON.parse(readFileSync(new URL('fixtures-manifest.json',dir),'utf8'));
 existing.errors={sha256:sha,bytes:built.font.length,tables:built.manifest,baseGlyphs:bases.map(x=>x[0]),
  badCases:{selfCycle:2,missingRef:3,unsupportedFormat:4,unsupportedMode:5},goodBase:6};
 writeFileSync(new URL('fixtures-manifest.json',dir),JSON.stringify(existing,null,2)+'\n');
 writeFileSync(new URL('errors.ttf',dir),built.font);
}
function verifyFixtures(){
 const dir=new URL('./fixtures/',import.meta.url);
 const man=JSON.parse(readFileSync(new URL('fixtures-manifest.json',dir),'utf8'));
 for(const [nm,key] of [['contract.ttf','contract'],['errors.ttf','errors']]){
  const bytes=readFileSync(new URL(nm,dir));const h=createHash('sha256').update(bytes).digest('hex');
  if(h!==man[key].sha256||bytes.length!==man[key].bytes)throw Error('fixture drift: '+nm);
 }
 console.log('FIXTURES_REPRO_OK fonts=2');
}
if(process.argv.includes('--check'))verifyFixtures();
