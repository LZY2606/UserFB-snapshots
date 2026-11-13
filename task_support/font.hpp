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
inline void u16(Bytes&b,unsigned n){b.push_back((n>>8)&255);b.push_back(n&255);}
inline void u32(Bytes&b,unsigned n){u16(b,n>>16);u16(b,n&65535);}
inline void set16(Bytes&b,size_t p,unsigned n){b.at(p)=(n>>8)&255;b.at(p+1)=n&255;}
inline void set32(Bytes&b,size_t p,unsigned n){set16(b,p,n>>16);set16(b,p+2,n&65535);}
inline unsigned sum(Bytes b){while(b.size()%4)b.push_back(0);unsigned n=0;for(size_t i=0;i<b.size();i+=4)n+=unsigned(b[i])<<24|unsigned(b[i+1])<<16|unsigned(b[i+2])<<8|unsigned(b[i+3]);return n;}
// Small, self-contained TrueType input builder. No coverage or fill-rule logic.
inline Bytes font(const std::vector<Contour>&cs){
 if(cs.empty())throw std::runtime_error("empty input contours");
 std::vector<Point>ps;for(auto&c:cs){if(c.empty())throw std::runtime_error("empty contour");ps.insert(ps.end(),c.begin(),c.end());}
 if(ps.size()>32000)throw std::runtime_error("fixture too large");
 int xmin=ps[0].x,xmax=xmin,ymin=ps[0].y,ymax=ymin;
 for(auto p:ps){xmin=std::min(xmin,p.x);xmax=std::max(xmax,p.x);ymin=std::min(ymin,p.y);ymax=std::max(ymax,p.y);}
 Bytes g;u16(g,cs.size());for(int v:{xmin,ymin,xmax,ymax})u16(g,v);unsigned e=0;for(auto&c:cs){e+=c.size();u16(g,e-1);}u16(g,0);
 for(auto p:ps)g.push_back(p.on?1:0);
 int prev=0;for(auto p:ps){u16(g,p.x-prev);prev=p.x;}prev=0;for(auto p:ps){u16(g,p.y-prev);prev=p.y;}
 if(g.size()%2)g.push_back(0);
 std::map<std::string,Bytes>ts;
 Bytes head(54);set32(head,0,0x10000);set32(head,12,0x5f0f3cf5);set16(head,18,64);set16(head,36,xmin);set16(head,38,ymin);set16(head,40,xmax);set16(head,42,ymax);set16(head,50,1);ts["head"]=head;
 Bytes hhea(36);set32(hhea,0,0x10000);set16(hhea,4,64);set16(hhea,34,2);ts["hhea"]=hhea;
 Bytes maxp(32);set32(maxp,0,0x10000);set16(maxp,4,2);set16(maxp,6,ps.size());set16(maxp,8,cs.size());set16(maxp,14,2);ts["maxp"]=maxp;
 Bytes loca(12);set32(loca,8,g.size());ts["loca"]=loca;ts["glyf"]=g;
 Bytes hmtx;for(int i=0;i<2;i++){u16(hmtx,52);u16(hmtx,xmin);}ts["hmtx"]=hmtx;
 Bytes cmap(24);set16(cmap,2,1);set16(cmap,4,3);set16(cmap,6,1);set32(cmap,8,12);set16(cmap,12,6);set16(cmap,14,12);set16(cmap,18,65);set16(cmap,20,1);set16(cmap,22,1);ts["cmap"]=cmap;
 Bytes out(12+16*ts.size());set32(out,0,0x10000);set16(out,4,ts.size());set16(out,6,64);set16(out,8,2);set16(out,10,ts.size()*16-64);
 size_t i=0,ho=0;for(auto&kv:ts){size_t p=12+16*i++;std::copy(kv.first.begin(),kv.first.end(),out.begin()+p);set32(out,p+4,sum(kv.second));set32(out,p+8,out.size());set32(out,p+12,kv.second.size());if(kv.first=="head")ho=out.size();out.insert(out.end(),kv.second.begin(),kv.second.end());while(out.size()%4)out.push_back(0);}
 set32(out,ho+8,0xb1b0afba-sum(out));return out;
}
}
