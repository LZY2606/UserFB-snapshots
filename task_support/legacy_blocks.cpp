#include "rgbcx.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int main(){
 rgbcx::init();unsigned char pixels[64],original[64],block[16],hq[16],decoded[64];
 for(int i=0;i<16;i++){pixels[4*i]=static_cast<unsigned char>(80+i*6);pixels[4*i+1]=static_cast<unsigned char>(100+(i%4)*17);pixels[4*i+2]=191;pixels[4*i+3]=255;}
 memcpy(original,pixels,64);memset(decoded,77,64);
 rgbcx::encode_bc5(block,pixels);rgbcx::encode_bc5_hq(hq,pixels);rgbcx::unpack_bc5(hq,decoded);
 assert(!memcmp(original,pixels,64));unsigned err=0;
 for(int i=0;i<16;i++){assert(decoded[4*i+2]==77&&decoded[4*i+3]==77);int x=decoded[4*i]-pixels[4*i],y=decoded[4*i+1]-pixels[4*i+1];err+=x*x+y*y;}
 assert(err>0&&err<2000);printf("BASELINE_BC5_PASS pixels=16 sentinel=32\n");
}
