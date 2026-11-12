// note: Typr runs inside a vm context (checks/load.cjs), so its arrays/objects
// have different prototypes - use loose deepEqual instead of deepStrictEqual
const test=require('node:test'), assert=require('node:assert');
const Typr=require('./load.cjs');
const {makeFont}=require('./makefont.cjs');

const fontBytes=makeFont();
const font=Typr.parse(new Uint8Array(fontBytes).buffer)[0];

// pen positions and offsets below are computed by hand from the known
// advances (A=600, B=620, marks=0), the kern pair (A,B)=-80 and the anchors
// written into the generated font; they never call the GPOS positioner.

test('gpos parse tables, anchors, coverage, classes',()=>{
	assert.ok(font.GPOS,'GPOS parsed');
	assert.deepEqual(font.GDEF,[0,1,1,3,3,1],'GDEF glyph classes');

	const g=font.GPOS;
	assert.deepEqual(Object.keys(g.scripts).sort(),['DFLT','latn']);
	assert.ok(g.scripts['latn'].dflt,'latn default LangSys');
	assert.ok(g.scripts['latn'].langs['TRK '],'latn TRK LangSys');
	assert.equal(g.scripts['DFLT'].dflt.req,1,'DFLT required feature = mkmk');

	assert.deepEqual(g.features.map(f=>f.tag),['mark','mkmk']);
	assert.deepEqual(g.features[0].lookups,[0,1,3]);
	assert.deepEqual(g.features[1].lookups,[2]);

	assert.equal(g.lookups.length,4);
	assert.deepEqual(g.lookups.map(l=>l.type),[4,2,9,4]);
	assert.equal(g.lookups[1].sub[0],null,'unsupported lookup type skipped, not fatal');
	assert.equal(g.lookups[2].sub[0].kind,6,'extension lookup resolved to mark-to-mark');

	const l0=g.lookups[0].sub[0];
	assert.equal(l0.kind,4);
	assert.deepEqual(l0.markCov.ids,[3]);
	assert.deepEqual(l0.baseCov.ids,[1,2,5]);
	assert.deepEqual(l0.marks[0],{cls:0,anc:{x:-100,y:750}});
	assert.deepEqual(l0.bases[0][0],{x:300,y:700});
	assert.deepEqual(l0.bases[1][0],{bad:2},'unsupported anchor format kept as bad');
	assert.equal(l0.bases[2][0],null,'null anchor = no attachment');

	const l2=g.lookups[2].sub[0];
	assert.deepEqual(l2.m1Cov.ids,[4]);
	assert.deepEqual(l2.marks1[0],{cls:1,anc:{x:-80,y:700}});
	assert.deepEqual(l2.marks2[0],[{x:10,y:790},{x:0,y:800}],'two mark classes');
});

test('gpos parse from a table at a non-zero buffer offset',()=>{
	const pad=64, big=new Uint8Array(fontBytes.length+pad);
	big.set(fontBytes,pad);
	const view=new Uint8Array(big.buffer,pad,fontBytes.length); // byteOffset!=0
	assert.notEqual(view.byteOffset,0);

	const gt=Typr.findTable(view,'GPOS',0), dt=Typr.findTable(view,'GDEF',0);
	const gpos=Typr.T.GPOS.parseTab(view,gt[0],gt[1],{});
	const gdef=Typr.T.GDEF.parseTab(view,dt[0],dt[1],{'maxp':{'numGlyphs':6}});
	assert.deepEqual(gpos,font.GPOS);
	assert.deepEqual(gdef,font.GDEF);
});

test('gpos anchors mark-to-base with kern and null anchors',()=>{
	// "AB́": kern moves B to pen 520, mark pen = 520+620 = 1140.
	// L0 base anchor of B is an unsupported format -> skipped;
	// L3 attaches gid3 to B(400,680) with mark anchor (-100,750):
	// dx = 520+400-(1140-100) = -120, dy = 680-750 = -70.
	const s=Typr.U.shape(font,'AB́');
	assert.deepEqual(s.map(g=>g.g),[1,2,3]);
	assert.deepEqual(s.map(g=>g.ax),[520,620,0],'kern applied to advance of A');
	assert.equal(s[2].dx,-120);
	assert.equal(s[2].dy,-70);
	assert.equal(s[0].dx,0); assert.equal(s[1].dx,0);

	// "Ć": C has a null anchor in L0 and is not in L3 -> mark keeps its place
	const s2=Typr.U.shape(font,'Ć');
	assert.deepEqual(s2.map(g=>g.g),[5,3]);
	assert.equal(s2[1].dx,0);
	assert.equal(s2[1].dy,0);

	// prm.gpos=false disables positioning entirely
	const s3=Typr.U.shape(font,'Á',{'gpos':false});
	assert.equal(s3[1].dx,0);
	assert.equal(s3[1].dy,0);
	assert.equal(s3[0].ax,600);
});

test('gpos chain mark-to-mark follows re-attached parent',()=>{
	// "Á̇": L0 attaches gid3 to A(300,700), L2 attaches gid4 to gid3,
	// L3 re-attaches gid3 to A(500,650) replacing L0.  Pens: 0, 600, 600.
	// gid3: dx = 500-(600-100) = 0, dy = 650-750 = -100.
	// gid4 follows gid3's final position: parent anchor (0,800), child anchor (-80,700):
	// dx = 600+0+0-(600-80) = 80, dy = -100+800-700 = 0.
	const s=Typr.U.shape(font,'Á̇');
	assert.deepEqual(s.map(g=>g.g),[1,3,4]);
	assert.equal(s[1].dx,0);  assert.equal(s[1].dy,-100);
	assert.equal(s[2].dx,80); assert.equal(s[2].dy,0);

	// language "TRK " has only "mark": the mkmk lookup never runs
	const t=Typr.U.shape(font,'Á̇',{'language':'TRK '});
	assert.equal(t[1].dy,-100);
	assert.equal(t[2].dx,0); assert.equal(t[2].dy,0);

	// unknown language falls back to the default LangSys
	const u=Typr.U.shape(font,'Á̇',{'language':'xxxx'});
	assert.equal(u[2].dx,80); assert.equal(u[1].dy,-100);

	// unknown script falls back to DFLT, whose LangSys pulls mkmk in as a required feature
	const v=Typr.U.shape(font,'Á̇',{'script':'cyrl'});
	assert.equal(v[1].dy,-100);
	assert.equal(v[2].dx,80); assert.equal(v[2].dy,0);

	// repeated shaping of the same font is stable and isolated from caller mutations
	const a=Typr.U.shape(font,'Á̇');
	const b=Typr.U.shape(font,'Á̇');
	assert.deepEqual(a,b);
	a[1].dx=999; a[2].dy=999;
	const c=Typr.U.shape(font,'Á̇');
	assert.equal(c[1].dx,0); assert.equal(c[2].dy,0);
	assert.equal(font.GPOS.lookups.length,4,'font tables not polluted');
});

test('gpos path shapeToPath yields translated outlines',()=>{
	const s=Typr.U.shape(font,'Á̇');
	const p=Typr.U.shapeToPath(font,s);
	// glyph outlines are axis-aligned rectangles; expected absolute coordinates
	// come from the known pen positions (0 / 600 / 600) plus dx,dy (0,0 / 0,-100 / 80,0)
	assert.deepEqual(p.cmds,['M','L','L','L','L','Z','M','L','L','L','L','Z','M','L','L','L','L','Z']);
	assert.deepEqual(p.crds,[
		50,700, 50,0, 450,0, 450,700, 50,700,                 // A at pen 0
		440,730, 440,630, 540,630, 540,730, 440,730,          // mark 1 at (600,-100)
		540,860, 540,760, 640,760, 640,860, 540,860           // mark 2 at (680,0)
	]);
});
