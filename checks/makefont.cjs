// Generates a minimal but valid SFNT (TrueType) font exercising GPOS mark positioning.
// Glyphs: 0=.notdef 1='A' 2='B' 3=U+0301 (mark, class 0) 4=U+0307 (mark, class 1) 5='C'
// kern: pair (A,B) -> -80.  GDEF: 1,2,5 base; 3,4 mark.
// GPOS scripts: latn (default LangSys: mark+mkmk; "TRK ": mark only), DFLT (mark + required mkmk).
// Lookups (LookupList order):
//   L0 type4  mark->base: gid3 -> A(300,700) / B(anchor format 2, unsupported) / C(null anchor)
//   L1 type2  unsupported lookup type, sandwiched between valid ones
//   L2 type9  Extension -> type6 mark->mark: gid4 (class 1) -> gid3 anchors [(10,790),(0,800)]
//   L3 type4  mark->base: gid3 -> A(500,650) / B(400,680)   (re-attaches gid3 after L2)

function u16(a,v){a.push((v>>8)&255,v&255);}
function s16(a,v){v&=0xffff;a.push((v>>8)&255,v&255);}
function u32(a,v){a.push((v>>>24)&255,(v>>>16)&255,(v>>>8)&255,v&255);}
function tag(a,s){for(var i=0;i<4;i++)a.push(s.charCodeAt(i));}
function cat(){var o=[];for(var i=0;i<arguments.length;i++)o=o.concat(arguments[i]);return o;}

function anchor1(x,y){var a=[];u16(a,1);s16(a,x);s16(a,y);return a;}
function anchor2(x,y,p){var a=[];u16(a,2);s16(a,x);s16(a,y);u16(a,p);return a;}

function cov1(ids){var a=[];u16(a,1);u16(a,ids.length);for(var i=0;i<ids.length;i++)u16(a,ids[i]);return a;}

function markArray(marks){ // marks: [{cls, anc:byteArray|null}]
	var a=[];u16(a,marks.length);
	var off=2+4*marks.length, tail=[];
	for(var i=0;i<marks.length;i++){
		u16(a,marks[i].cls);
		if(marks[i].anc){u16(a,off);tail=tail.concat(marks[i].anc);off+=marks[i].anc.length;}
		else u16(a,0);
	}
	return a.concat(tail);
}
function anchorMatrix(recs,classCnt){ // BaseArray / Mark2Array
	var a=[];u16(a,recs.length);
	var off=2+2*classCnt*recs.length, tail=[];
	for(var i=0;i<recs.length;i++)for(var c=0;c<classCnt;c++){
		var anc=recs[i][c];
		if(anc){u16(a,off);tail=tail.concat(anc);off+=anc.length;}
		else u16(a,0);
	}
	return a.concat(tail);
}
function markBasePos(p){ // {markCov, baseCov, classCnt, marks, bases}
	var mc=cov1(p.markCov), bc=cov1(p.baseCov), ma=markArray(p.marks), ba=anchorMatrix(p.bases,p.classCnt);
	var a=[];u16(a,1);
	u16(a,12);u16(a,12+mc.length);u16(a,p.classCnt);
	u16(a,12+mc.length+bc.length);u16(a,12+mc.length+bc.length+ma.length);
	return a.concat(mc,bc,ma,ba);
}
function markMarkPos(p){ // {m1Cov, m2Cov, classCnt, marks1, marks2}
	var c1=cov1(p.m1Cov), c2=cov1(p.m2Cov), m1=markArray(p.marks1), m2=anchorMatrix(p.marks2,p.classCnt);
	var a=[];u16(a,1);
	u16(a,12);u16(a,12+c1.length);u16(a,p.classCnt);
	u16(a,12+c1.length+c2.length);u16(a,12+c1.length+c2.length+m1.length);
	return a.concat(c1,c2,m1,m2);
}
function extension(extType,sub){var a=[];u16(a,1);u16(a,extType);u32(a,8);return a.concat(sub);}
function lookup(type,flag,subs){
	var a=[];u16(a,type);u16(a,flag);u16(a,subs.length);
	var off=6+2*subs.length, tail=[];
	for(var i=0;i<subs.length;i++){u16(a,off);off+=subs[i].length;tail=tail.concat(subs[i]);}
	return a.concat(tail);
}
function langSys(req,feats){
	var a=[];u16(a,0);u16(a,req<0?0xffff:req);u16(a,feats.length);
	for(var i=0;i<feats.length;i++)u16(a,feats[i]);
	return a;
}
function script(dflt,langs){ // langs: [[tag, langSysBytes]]
	var hdr=4+6*langs.length, off=hdr, body=[];
	var dOff=0;
	if(dflt){dOff=off;off+=dflt.length;}
	var a=[];u16(a,dOff);u16(a,langs.length);
	for(var i=0;i<langs.length;i++){tag(a,langs[i][0]);u16(a,off);off+=langs[i][1].length;}
	if(dflt)body=body.concat(dflt);
	for(var i=0;i<langs.length;i++)body=body.concat(langs[i][1]);
	return a.concat(body);
}
function scriptList(scrs){
	var a=[];u16(a,scrs.length);
	var off=2+6*scrs.length, body=[];
	for(var i=0;i<scrs.length;i++){tag(a,scrs[i][0]);u16(a,off);off+=scrs[i][1].length;body=body.concat(scrs[i][1]);}
	return a.concat(body);
}
function feature(lups){var a=[];u16(a,0);u16(a,lups.length);for(var i=0;i<lups.length;i++)u16(a,lups[i]);return a;}
function featureList(fts){
	var a=[];u16(a,fts.length);
	var off=2+6*fts.length, body=[];
	for(var i=0;i<fts.length;i++){tag(a,fts[i][0]);u16(a,off);off+=fts[i][1].length;body=body.concat(fts[i][1]);}
	return a.concat(body);
}
function lookupList(lups){
	var a=[];u16(a,lups.length);
	var off=2+2*lups.length, body=[];
	for(var i=0;i<lups.length;i++){u16(a,off);off+=lups[i].length;body=body.concat(lups[i]);}
	return a.concat(body);
}
function gpos(sl,fl,ll){
	var a=[];u16(a,1);u16(a,0);
	u16(a,10);u16(a,10+sl.length);u16(a,10+sl.length+fl.length);
	return a.concat(sl,fl,ll);
}

function rectGlyph(x0,y0,x1,y1){
	var a=[];
	s16(a,1);s16(a,x0);s16(a,y0);s16(a,x1);s16(a,y1);
	u16(a,3);u16(a,0);       // endPtsOfContours, instructionLength
	a.push(1,1,1,1);          // flags: on-curve, 16-bit deltas
	s16(a,x0);s16(a,x1-x0);s16(a,0);s16(a,x0-x1);
	s16(a,y0);s16(a,0);s16(a,y1-y0);s16(a,0);
	return a;
}

function checksum(b){
	var sum=0;
	for(var i=0;i<b.length;i+=4){
		var v=(((b[i]||0)<<24)|((b[i+1]||0)<<16)|((b[i+2]||0)<<8)|(b[i+3]||0))>>>0;
		sum=(sum+v)>>>0;
	}
	return sum;
}

function buildSFNT(tabs){ // tabs: [[tag, bytes]]
	tabs.sort(function(a,b){return a[0]<b[0]?-1:1;});
	var n=tabs.length, maxPow=1, es=0;
	while(maxPow*2<=n){maxPow*=2;es++;}
	var head=[];u32(head,0x10000);u16(head,n);u16(head,maxPow*16);u16(head,es);u16(head,n*16-maxPow*16);
	var off=12+16*n, recs=[];
	for(var i=0;i<n;i++){
		while(off%4)off++;
		recs.push([tabs[i][0],checksum(tabs[i][1]),off,tabs[i][1].length]);
		off+=tabs[i][1].length;
	}
	var dir=[];
	for(var i=0;i<n;i++){tag(dir,recs[i][0]);u32(dir,recs[i][1]);u32(dir,recs[i][2]);u32(dir,recs[i][3]);}
	var font=head.concat(dir);
	for(var i=0;i<n;i++){
		while(font.length%4)font.push(0);
		font=font.concat(tabs[i][1]);
	}
	var adj=(0xB1B0AFBA-checksum(font))>>>0;
	for(var i=0;i<n;i++)if(recs[i][0]=="head"){
		var ho=recs[i][2];
		font[ho+8]=(adj>>>24)&255;font[ho+9]=(adj>>>16)&255;font[ho+10]=(adj>>>8)&255;font[ho+11]=adj&255;
	}
	return font;
}

function makeFont(){
	// --- glyf / loca ---
	var glyphs=[
		[],
		rectGlyph(  50,  0, 450, 700),   // 1 'A'
		rectGlyph(  60,  0, 460, 700),   // 2 'B'
		rectGlyph(-160,730, -60, 830),   // 3 U+0301 mark (class 0)
		rectGlyph(-140,760, -40, 860),   // 4 U+0307 mark (class 1)
		rectGlyph(  55,  0, 455, 700)    // 5 'C'
	];
	var glyf=[], loca=[], offs=[0];
	for(var i=0;i<glyphs.length;i++){glyf=glyf.concat(glyphs[i]);offs.push(glyf.length);}
	for(var i=0;i<offs.length;i++)u16(loca,offs[i]>>1);

	// --- head ---
	var head=[];u32(head,0x10000);u32(head,0x10000);u32(head,0);u32(head,0x5F0F3CF5);
	u16(head,0);u16(head,1000);u32(head,0);u32(head,0);u32(head,0);u32(head,0);
	s16(head,-160);s16(head,0);s16(head,460);s16(head,860);
	u16(head,0);u16(head,8);s16(head,2);s16(head,0);s16(head,0);

	// --- hhea ---
	var hhea=[];u32(hhea,0x10000);
	s16(hhea,860);s16(hhea,-160);s16(hhea,0);
	u16(hhea,620);s16(hhea,0);s16(hhea,0);s16(hhea,460);
	s16(hhea,1);s16(hhea,0);s16(hhea,0);
	s16(hhea,0);s16(hhea,0);s16(hhea,0);s16(hhea,0);
	s16(hhea,0);u16(hhea,6);

	// --- maxp ---
	var maxp=[];u32(maxp,0x10000);u16(maxp,6);
	for(var i=0;i<13;i++)u16(maxp,0);

	// --- hmtx ---
	var hmtx=[], aws=[500,600,620,0,0,600];
	for(var i=0;i<6;i++){u16(hmtx,aws[i]);s16(hmtx,0);}

	// --- cmap (platform 3, encoding 1, format 4) ---
	var sub=[];
	u16(sub,4);u16(sub,56);u16(sub,0);
	u16(sub,10);u16(sub,8);u16(sub,2);u16(sub,2);
	var ends=[0x42,0x43,0x301,0x307,0xFFFF], starts=[0x41,0x43,0x301,0x307,0xFFFF], deltas=[-64,-62,-766,-771,1];
	for(var i=0;i<5;i++)u16(sub,ends[i]);
	u16(sub,0);
	for(var i=0;i<5;i++)u16(sub,starts[i]);
	for(var i=0;i<5;i++)s16(sub,deltas[i]);
	for(var i=0;i<5;i++)u16(sub,0);
	var cmap=[];u16(cmap,0);u16(cmap,1);u16(cmap,3);u16(cmap,1);u32(cmap,12);
	cmap=cmap.concat(sub);

	// --- kern (pair A,B -> -80) ---
	var kern=[];u16(kern,0);u16(kern,1);
	u16(kern,0);u16(kern,20);u16(kern,1);
	u16(kern,1);u16(kern,6);u16(kern,0);u16(kern,0);
	u16(kern,1);u16(kern,2);s16(kern,-80);

	// --- name (postScriptName) ---
	var name=[];u16(name,0);u16(name,1);u16(name,18);
	u16(name,3);u16(name,1);u16(name,0x0409);u16(name,6);u16(name,16);u16(name,0);
	var ps="TestGPOS";
	for(var i=0;i<ps.length;i++)u16(name,ps.charCodeAt(i));

	// --- post ---
	var post=[];u32(post,0x30000);u32(post,0);s16(post,-100);s16(post,50);
	u32(post,0);u32(post,0);u32(post,0);u32(post,0);u32(post,0);

	// --- GDEF ---
	var gdef=[];u16(gdef,1);u16(gdef,0);u16(gdef,12);u16(gdef,0);u16(gdef,0);u16(gdef,0);
	u16(gdef,2);u16(gdef,3);   // ClassDef format 2, 3 ranges
	u16(gdef,1);u16(gdef,2);u16(gdef,1);
	u16(gdef,3);u16(gdef,4);u16(gdef,3);
	u16(gdef,5);u16(gdef,5);u16(gdef,1);

	// --- GPOS ---
	var L0=lookup(4,0,[markBasePos({
		markCov:[3], baseCov:[1,2,5], classCnt:1,
		marks:[{cls:0,anc:anchor1(-100,750)}],
		bases:[[anchor1(300,700)],[anchor2(250,700,0)],[null]]
	})]);
	var L1=lookup(2,0,[[0,1,0,0,0,0]]);   // unsupported lookup type
	var L2=lookup(9,0,[extension(6,markMarkPos({
		m1Cov:[4], m2Cov:[3], classCnt:2,
		marks1:[{cls:1,anc:anchor1(-80,700)}],
		marks2:[[anchor1(10,790),anchor1(0,800)]]
	}))]);
	var L3=lookup(4,0,[markBasePos({
		markCov:[3], baseCov:[1,2], classCnt:1,
		marks:[{cls:0,anc:anchor1(-100,750)}],
		bases:[[anchor1(500,650)],[anchor1(400,680)]]
	})]);

	var sl=scriptList([
		["DFLT",script(langSys(1,[0]),[])],                    // required feature: mkmk
		["latn",script(langSys(-1,[0,1]),[["TRK ",langSys(-1,[0])]])]
	]);
	var fl=featureList([["mark",feature([0,1,3])],["mkmk",feature([2])]]);
	var ll=lookupList([L0,L1,L2,L3]);
	var GPOS=gpos(sl,fl,ll);

	return buildSFNT([
		["GDEF",gdef],["GPOS",GPOS],["cmap",cmap],["glyf",glyf],["head",head],["hhea",hhea],
		["hmtx",hmtx],["kern",kern],["loca",loca],["maxp",maxp],["name",name],["post",post]
	]);
}

module.exports={makeFont:makeFont};
