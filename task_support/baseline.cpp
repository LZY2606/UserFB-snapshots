#include "rdo_bc_encoder.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
namespace witness {
struct XY { double x,y; };
struct N { double x,y,z; };
N normal(XY p) { N n={2*p.x/255-1,2*p.y/255-1,0}; n.z=std::sqrt(std::max(0.0,1-n.x*n.x-n.y*n.y)); double l=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z); n.x/=l;n.y/=l;n.z/=l;return n; }
double loss(XY a,XY b) { N n=normal(a),m=normal(b);return std::max(0.0,1-n.x*m.x-n.y*m.y-n.z*m.z); }
unsigned channel(const unsigned char* p,unsigned pixel) {
 unsigned q[8]={p[0],p[1],0,0,0,0,0,0};
 if(q[0]>q[1])for(unsigned i=2;i<8;++i)q[i]=((8-i)*q[0]+(i-1)*q[1])/7;
 else {for(unsigned i=2;i<6;++i)q[i]=((6-i)*q[0]+(i-1)*q[1])/5;q[6]=0;q[7]=255;}
 uint64_t bits=0;for(unsigned i=0;i<6;++i)bits|=uint64_t(p[2+i])<<(8*i);
 return q[(bits>>(3*pixel))&7];
}
XY decoded(const std::vector<unsigned char>& b,int x,int y) {
 const unsigned char* p=&b[16*(x/4)];unsigned i=(y%4)*4+x%4;
 return XY{double(channel(p,i)),double(channel(p+8,i))};
}
XY sampled(const utils::image_u8& im,const std::vector<unsigned char>* b,double x,double y) {
 int ix=int(std::floor(x)),iy=int(std::floor(y));double tx=x-ix,ty=y-iy;XY s={0,0};
 for(int j=0;j<2;++j)for(int i=0;i<2;++i){int xx=((ix+i)%8+8)%8,yy=((iy+j)%4+4)%4;
 XY a=b?decoded(*b,xx,yy):XY{double(im(xx,yy).r),double(im(xx,yy).g)};
 double w=(i?tx:1-tx)*(j?ty:1-ty);s.x+=w*a.x;s.y+=w*a.y;}
 return s;
}
utils::image_u8 source(){utils::image_u8 im(8,4);for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x)
 im(x,y)=utils::color_quad_u8(65+(x*19+y*11)%135,75+(x*13+y*29)%127,77,255);return im;}
std::vector<unsigned char> encode(const utils::image_u8& im){rdo_bc::rdo_bc_params p;p.m_dxgi_format=DXGI_FORMAT_BC5_UNORM;p.m_use_hq_bc345=true;p.m_bc345_search_rad=rgbcx::BC4_DEFAULT_SEARCH_RAD;p.m_rdo_lambda=0;p.m_status_output=false;
 rdo_bc::rdo_bc_encoder e;assert(e.init(im,p)&&e.encode());const unsigned char* b=(const unsigned char*)e.get_blocks();return std::vector<unsigned char>(b,b+32);}
}
#ifndef WITNESS_NO_MAIN
int main(){using namespace witness;utils::image_u8 im=source(),original=im;auto b=encode(im);unsigned char legacy[32];
 for(unsigned bx=0;bx<2;++bx){utils::color_quad_u8 pix[16];im.get_block(bx,0,4,4,pix);rgbcx::encode_bc5_hq(legacy+16*bx,(const unsigned char*)pix);}
 assert(!std::memcmp(b.data(),legacy,32));
 for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x)assert(!std::memcmp(&im(x,y),&original(x,y),4));
 double a=loss(sampled(im,nullptr,3.75,1.25),sampled(im,&b,3.75,1.25));
 double c=loss(sampled(im,nullptr,7.75,2.5),sampled(im,&b,7.75,2.5));
 assert(std::isfinite(a)&&a>0&&std::isfinite(c)&&c>0);
 uint64_t h=1469598103934665603ULL;for(auto v:b)h=(h^v)*1099511628211ULL;
 std::printf("LEGACY_BC5_PASS blocks=2 checksum=%016llx\n",(unsigned long long)h);
 std::printf("FILTER_WITNESS cross_block=%.17g repeat_seam=%.17g\n",a,c);
 std::puts("BASELINE_SAMPLED_NORMAL_PASS samples=2 source_unchanged=32");
}
#endif
