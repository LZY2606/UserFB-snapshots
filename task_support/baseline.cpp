#include "font.hpp"
#include "schrift.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <memory>
using fixture::Contour;
struct FreeFont {void operator()(SFT_Font*f)const{sft_freefont(f);}};
using Font=std::unique_ptr<SFT_Font,FreeFont>;
std::vector<unsigned char> render(SFT_Font*f,double scale,int flags,SFT_GMetrics&m){
 SFT s={f,scale,scale,0,0,flags};SFT_Glyph g=0;
 assert(!sft_lookup(&s,65,&g)&&g==1);assert(!sft_gmetrics(&s,g,&m));
 std::vector<unsigned char>p(size_t(m.minWidth)*m.minHeight,0xA5);
 assert(!sft_render(&s,g,{p.data(),m.minWidth,m.minHeight}));return p;
}
int main(int argc,char**argv){
 assert(argc==2);
 auto bytes=fixture::font({{{1,0},{41,0},{41,45},{1,45}}});
 std::ofstream out(argv[1],std::ios::binary);out.write((char*)bytes.data(),bytes.size());out.close();
 Font a(sft_loadfile(argv[1])),b(sft_loadmem(bytes.data(),bytes.size()));assert(a&&b);
 SFT_GMetrics m,n;auto p=render(a.get(),16,SFT_DOWNWARD_Y,m),q=render(b.get(),16,SFT_DOWNWARD_Y,n);assert(p==q);
 unsigned count=0,sum=0;for(auto v:p){count+=v!=0;sum+=v;}
 assert(m.advanceWidth==13&&m.minWidth==12&&m.minHeight==13&&count==132&&sum==28690);
 printf("LEGACY_RECT_OK size=12x13 nonzero=132 sum=28690 file=memory\n");
 auto dbl=fixture::font({{{0,0},{64,0},{64,64},{0,64}},{{0,0},{64,0},{64,64},{0,64}}});
 Font d(sft_loadmem(dbl.data(),dbl.size()));assert(d);auto dp=render(d.get(),1,0,m);assert(dp[0]==255);
 printf("LEGACY_DOUBLE_SQUARE pixel00=%u\n",dp[0]);
 auto bow=fixture::font({{{0,0},{64,64},{0,64},{64,0}}});Font t(sft_loadmem(bow.data(),bow.size()));assert(t);auto tp=render(t.get(),1,0,m);
 printf("LEGACY_BOWTIE pixel00=%u\n",tp[0]);assert(tp[0]==0);
 puts("SCHRIFT_BASELINE_COMPLETE");
}
