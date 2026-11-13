#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
namespace fixture {
using Bytes=std::vector<unsigned char>;
struct Point { int x,y; bool on=true; };
using Contour=std::vector<Point>;
struct Component {
 uint16_t glyph; int dx,dy;
 bool scaled=false; double sx=1.0,sy=1.0;
};
struct GlyphBuild {
 std::vector<Contour> simple;
 std::vector<Component> compound;
 bool isCompound=false;
};
inline void u16(Bytes&b,unsigned n){b.push_back((n>>8)&255);b.push_back(n&255);}
inline void u32(Bytes&b,unsigned n){u16(b,n>>16);u16(b,n&65535);}
inline void set16(Bytes&b,size_t p,unsigned n){b.at(p)=(n>>8)&255;b.at(p+1)=n&255;}
inline void set32(Bytes&b,size_t p,unsigned n){set16(b,p,n>>16);set16(b,p+2,n&65535);}
inline unsigned sum(Bytes b){while(b.size()%4)b.push_back(0);unsigned n=0;for(size_t i=0;i<b.size();i+=4)n+=unsigned(b[i])<<24|unsigned(b[i+1])<<16|unsigned(b[i+2])<<8|unsigned(b[i+3]);return n;}

struct GBBox {int xmin,ymin,xmax,ymax;};
inline GBBox contours_bbox(const std::vector<Contour>&cs){
 std::vector<Point>ps;for(auto&c:cs)ps.insert(ps.end(),c.begin(),c.end());
 if(ps.empty())return {0,0,0,0};
 int xmin=ps[0].x,xmax=xmin,ymin=ps[0].y,ymax=ymin;
 for(auto p:ps){xmin=std::min(xmin,p.x);xmax=std::max(xmax,p.x);ymin=std::min(ymin,p.y);ymax=std::max(ymax,p.y);}
 return {xmin,ymin,xmax,ymax};
}
// Encodes one simple glyph (including its 10-byte header).
inline Bytes encode_simple(const std::vector<Contour>&cs){
 std::vector<Point>ps;for(auto&c:cs){if(c.empty())throw std::runtime_error("empty contour");ps.insert(ps.end(),c.begin(),c.end());}
 if(ps.size()>32000)throw std::runtime_error("fixture too large");
 GBBox bb=contours_bbox(cs);
 Bytes g;u16(g,cs.size());for(int v:{bb.xmin,bb.ymin,bb.xmax,bb.ymax})u16(g,(unsigned)(v+65536));
 unsigned e=0;for(auto&c:cs){e+=(unsigned)c.size();u16(g,e-1);}u16(g,0);
 for(auto p:ps)g.push_back(p.on?1:0);
 int prev=0;for(auto p:ps){u16(g,(unsigned)(p.x-prev+65536));prev=p.x;}prev=0;for(auto p:ps){u16(g,(unsigned)(p.y-prev+65536));prev=p.y;}
 if(g.size()%2)g.push_back(0);
 return g;
}
// Encodes one compound glyph. Referenced component glyphs must be encoded too.
inline Bytes encode_compound(const std::vector<Component>&comps,const std::vector<GBBox>&boxes){
 int xmin=0,ymin=0,xmax=0,ymax=0;bool first=true;
 for(auto&c:comps){
  GBBox b=boxes.at(c.glyph);
  int x0=(int)std::floor(b.xmin*(c.scaled?c.sx:1.0))+c.dx;
  int x1=(int)std::ceil (b.xmax*(c.scaled?c.sx:1.0))+c.dx;
  int y0=(int)std::floor(b.ymin*(c.scaled?c.sy:1.0))+c.dy;
  int y1=(int)std::ceil (b.ymax*(c.scaled?c.sy:1.0))+c.dy;
  if(first){xmin=x0;xmax=x1;ymin=y0;ymax=y1;first=false;}
  else{xmin=std::min(xmin,x0);xmax=std::max(xmax,x1);ymin=std::min(ymin,y0);ymax=std::max(ymax,y1);}
 }
 Bytes g;u16(g,(unsigned)-1);for(int v:{xmin,ymin,xmax,ymax})u16(g,(unsigned)(v+65536));
 for(size_t i=0;i<comps.size();++i){
  auto&c=comps[i];
  unsigned flags=0x0001/*ARG_16_AND_ARE_WORDS*/|0x0002/*ARGS_ARE_XY_VALUES*/;
  if(i+1<comps.size())flags|=0x0020/*MORE_COMPONENTS*/;
  if(c.scaled)flags|=0x0040/*WE_HAVE_AN_X_AND_Y_SCALE*/;
  u16(g,flags);u16(g,c.glyph);u16(g,(unsigned)(c.dx+65536));u16(g,(unsigned)(c.dy+65536));
  if(c.scaled){u16(g,(unsigned)std::lround(c.sx*16384.0));u16(g,(unsigned)std::lround(c.sy*16384.0));}
 }
 if(g.size()%2)g.push_back(0);
 return g;
}
// Small, self-contained TrueType input builder. No coverage or fill-rule logic.
inline Bytes font_multi(const std::vector<GlyphBuild>&glyphs,
                        const std::vector<std::pair<unsigned,unsigned>>&mapping);
inline Bytes font(const std::vector<Contour>&cs){
 if(cs.empty())throw std::runtime_error("empty input contours");
 GlyphBuild g;g.simple=cs;
 return font_multi({GlyphBuild{},g},{{65,1}});
}
// General builder: glyph 0 is kept empty (`.notdef`), further slots hold the
// supplied glyphs. `mapping` maps Unicode code points to glyph ids.
inline Bytes font_multi(const std::vector<GlyphBuild>&glyphs,
                        const std::vector<std::pair<unsigned,unsigned>>&mapping){
 if(glyphs.size()<2)throw std::runtime_error("need at least .notdef");
 size_t n=glyphs.size();
 std::vector<Bytes> bodies(n);
 std::vector<GBBox> boxes(n,{0,0,0,0});
 unsigned maxPoints=0,maxContours=0,maxCompPoints=0,maxCompContours=0;
 for(size_t i=1;i<n;++i){
  if(glyphs[i].isCompound){
   bodies[i]=encode_compound(glyphs[i].compound,boxes);
   maxCompContours=std::max(maxCompContours,(unsigned)glyphs[i].compound.size());
   unsigned cp=0;for(auto&c:glyphs[i].compound)cp=std::max(cp,(unsigned)boxes.at(c.glyph).xmax);
   maxCompPoints=std::max(maxCompPoints,cp);
  }else if(!glyphs[i].simple.empty()){
   bodies[i]=encode_simple(glyphs[i].simple);
   boxes[i]=contours_bbox(glyphs[i].simple);
   unsigned pc=0;for(auto&c:glyphs[i].simple)pc+=(unsigned)c.size();
   maxPoints=std::max(maxPoints,pc);
   maxContours=std::max(maxContours,(unsigned)glyphs[i].simple.size());
  }
 }
 Bytes glyf;std::vector<unsigned>loca;loca.push_back(0);
 for(size_t i=0;i<n;++i){glyf.insert(glyf.end(),bodies[i].begin(),bodies[i].end());loca.push_back((unsigned)glyf.size());}
 std::map<std::string,Bytes>ts;
 int fxmin=0,fymin=0,fxmax=0,fymax=0;bool any=false;
 for(auto&b:boxes){if(b.xmax<=b.xmin)continue;if(!any){fxmin=b.xmin;fymin=b.ymin;fxmax=b.xmax;fymax=b.ymax;any=true;}
  fxmin=std::min(fxmin,b.xmin);fymin=std::min(fymin,b.ymin);fxmax=std::max(fxmax,b.xmax);fymax=std::max(fymax,b.ymax);}
 Bytes head(54);set32(head,0,0x10000);set32(head,12,0x5f0f3cf5);set16(head,18,64);
 set16(head,36,(unsigned)(fxmin+65536));set16(head,38,(unsigned)(fymin+65536));set16(head,40,(unsigned)(fxmax+65536));set16(head,42,(unsigned)(fymax+65536));
 set16(head,50,1);ts["head"]=head;
 Bytes hhea(36);set32(hhea,0,0x10000);set16(hhea,4,64);set16(hhea,34,(unsigned)n);ts["hhea"]=hhea;
 Bytes maxp(32);set32(maxp,0,0x10000);set16(maxp,4,(unsigned)n);set16(maxp,6,std::max(maxPoints,1u));
 set16(maxp,8,std::max(maxContours,1u));set16(maxp,10,std::max(maxCompPoints,1u));set16(maxp,12,std::max(maxCompContours,1u));set16(maxp,14,2);ts["maxp"]=maxp;
 Bytes locaB;for(auto v:loca)u32(locaB,v);ts["loca"]=locaB;ts["glyf"]=glyf;
 Bytes hmtx;for(size_t i=0;i<n;++i){u16(hmtx,52);u16(hmtx,(unsigned)(boxes[i].xmin+65536));}ts["hmtx"]=hmtx;
 // Format 6 cmap covering the contiguous code point range of the mapping.
 unsigned first=65,last=65;if(!mapping.empty()){first=mapping.front().first;last=first;
  for(auto&kv:mapping){first=std::min(first,kv.first);last=std::max(last,kv.first);}}
 unsigned count=last-first+1;
 Bytes cmap;
 u16(cmap,0);u16(cmap,1);             /* cmap header */
 u16(cmap,3);u16(cmap,1);u32(cmap,12);/* encoding record: platform 3, enc 1 */
 u16(cmap,6);u16(cmap,10+2*count);u16(cmap,0);/* format-6 subtable */
 u16(cmap,first);u16(cmap,count);
 std::vector<unsigned>gids(count,0);for(auto&kv:mapping)gids.at(kv.first-first)=kv.second;
 for(auto v:gids)u16(cmap,v);ts["cmap"]=cmap;
 Bytes out(12+16*ts.size());set32(out,0,0x10000);set16(out,4,ts.size());set16(out,6,64);set16(out,8,2);set16(out,10,ts.size()*16-64);
 size_t i=0,ho=0;for(auto&kv:ts){size_t p=12+16*i++;std::copy(kv.first.begin(),kv.first.end(),out.begin()+p);set32(out,p+4,sum(kv.second));set32(out,p+8,out.size());set32(out,p+12,kv.second.size());if(kv.first=="head")ho=out.size();out.insert(out.end(),kv.second.begin(),kv.second.end());while(out.size()%4)out.push_back(0);}
 set32(out,ho+8,0xb1b0afba-sum(out));return out;
}
// A font whose first mapped glyph (A) cannot be decoded while glyph B remains
// renderable. The bad glyph is a compound referencing a glyph id far beyond
// the font, which fails outline-offset bounds checking deterministically.
inline Bytes corrupt_then_good_font(const std::vector<Contour>&good){
 Bytes goodBody=encode_simple(good);GBBox bb=contours_bbox(good);
 // Compound header (numContours = -1) followed by one component record that
 // omits the required ARGS_ARE_XY_VALUES flag, which the decoder rejects.
 Bytes bad;u16(bad,(unsigned)-1);for(int v:{bb.xmin,bb.ymin,bb.xmax,bb.ymax})u16(bad,(unsigned)(v+65536));
 u16(bad,0x0000);u16(bad,999);bad.push_back(0);bad.push_back(0);
 Bytes glyf;
 unsigned off0=0, off1=0, off2=(unsigned)bad.size(), off3=off2+(unsigned)goodBody.size();
 glyf.insert(glyf.end(),bad.begin(),bad.end());glyf.insert(glyf.end(),goodBody.begin(),goodBody.end());
 std::map<std::string,Bytes>ts;
 Bytes head(54);set32(head,0,0x10000);set32(head,12,0x5f0f3cf5);set16(head,18,64);
 set16(head,36,(unsigned)(bb.xmin+65536));set16(head,38,(unsigned)(bb.ymin+65536));set16(head,40,(unsigned)(bb.xmax+65536));set16(head,42,(unsigned)(bb.ymax+65536));
 set16(head,50,1);ts["head"]=head;
 Bytes hhea(36);set32(hhea,0,0x10000);set16(hhea,4,64);set16(hhea,34,3);ts["hhea"]=hhea;
 Bytes maxp(32);set32(maxp,0,0x10000);set16(maxp,4,3);set16(maxp,6,16);set16(maxp,8,2);set16(maxp,14,2);ts["maxp"]=maxp;
 Bytes loca;u32(loca,off0);u32(loca,off1);u32(loca,off2);u32(loca,off3);ts["loca"]=loca;ts["glyf"]=glyf;
 Bytes hmtx;for(int i=0;i<3;++i){u16(hmtx,52);u16(hmtx,(unsigned)(bb.xmin+65536));}ts["hmtx"]=hmtx;
 Bytes cmap;
 u16(cmap,0);u16(cmap,1);
 u16(cmap,3);u16(cmap,1);u32(cmap,12);
 u16(cmap,6);u16(cmap,14);u16(cmap,0);u16(cmap,65);u16(cmap,2);u16(cmap,1);u16(cmap,2);ts["cmap"]=cmap;
 Bytes out(12+16*ts.size());set32(out,0,0x10000);set16(out,4,ts.size());set16(out,6,64);set16(out,8,2);set16(out,10,ts.size()*16-64);
 size_t i=0,ho=0;for(auto&kv:ts){size_t p=12+16*i++;std::copy(kv.first.begin(),kv.first.end(),out.begin()+p);set32(out,p+4,sum(kv.second));set32(out,p+8,out.size());set32(out,p+12,kv.second.size());if(kv.first=="head")ho=out.size();out.insert(out.end(),kv.second.begin(),kv.second.end());while(out.size()%4)out.push_back(0);}
 set32(out,ho+8,0xb1b0afba-sum(out));return out;
}
}
