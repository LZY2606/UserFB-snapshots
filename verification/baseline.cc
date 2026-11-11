#include "libyuv.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main() {
  using namespace libyuv;
  const int w=8,h=2; uint16_t y[24],u[16],v[16],py[24],uv[24],oy[24],ou[16],ov[16];
  for(int i=0;i<24;i++) y[i]=(i*41)&1023;
  for(int i=0;i<16;i++) {u[i]=(i*93)&1023;v[i]=1023-u[i];}
  memset(oy,0x55,sizeof(oy));memset(ou,0x55,sizeof(ou));memset(ov,0x55,sizeof(ov));
  assert(!I210ToP210(y,12,u,8,v,8,py,12,uv,12,w,h));
  assert(!P210ToI210(py,12,uv,12,oy,12,ou,8,ov,8,w,h));
  for(int r=0;r<h;r++) {
    for(int x=0;x<w;x++) assert(y[r*12+x]==oy[r*12+x]);
    for(int x=0;x<w/2;x++) {assert(u[r*8+x]==ou[r*8+x]);assert(v[r*8+x]==ov[r*8+x]);}
    assert(oy[r*12+w]==0x5555);
  }
  unsigned char packed[256]={0},dy[16]={0},du[4]={0},dv[4]={0};
  // A valid v210 frame now converts through the dispatcher.
  assert(!I210ToV210(y,12,u,8,v,8,packed,128,w,h));
  assert(!ConvertToI420(packed,sizeof(packed),dy,8,du,4,dv,4,
       0,0,w,h,w,h,kRotate0,FOURCC('v','2','1','0')));
  // A sample buffer that cannot cover the full frame is still rejected.
  assert(ConvertToI420(packed,127,dy,8,du,4,dv,4,
       0,0,w,h,w,h,kRotate0,FOURCC('v','2','1','0'))==-1);
  puts("baseline: padded I210/P210 roundtrip exact; v210 dispatch converts and rejects short buffers");
}
