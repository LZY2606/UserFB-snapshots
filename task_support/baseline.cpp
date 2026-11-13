#include "rg_etc1.h"
#include <iostream>
int main(){
 unsigned src[16],out[16];auto*b=reinterpret_cast<unsigned char*>(src);
 for(int y=0;y<4;y++)for(int x=0;x<4;x++){int k=4*(4*y+x);b[k]=20+35*x;b[k+1]=30+30*y;b[k+2]=40+12*(x+y);b[k+3]=255;}
 alignas(unsigned long long) unsigned char block[8];rg_etc1::pack_etc1_block_init();rg_etc1::etc1_pack_params p;
 unsigned reported=rg_etc1::pack_etc1_block(block,src,p);
 if(!rg_etc1::unpack_etc1_block(block,out))return 1;
 auto*d=reinterpret_cast<unsigned char*>(out);unsigned measured=0;
 for(int k=0;k<64;k++)if(k%4!=3){int diff=int(b[k])-int(d[k]);measured+=diff*diff;}else if(d[k]!=255)return 2;
 if(measured!=reported)return 3;
 std::cout<<"ETC1_RAMP_ROUNDTRIP_OK error="<<measured<<"\n";
}
