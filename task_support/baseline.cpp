#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include <iostream>
#include <cstring>
struct Bits{unsigned char data[16]={};unsigned pos=0;void put(unsigned v,unsigned n){for(unsigned k=0;k<n;k++,pos++)data[pos/8]|=((v>>k)&1)<<(pos%8);}};
int main(){
 Bits b;b.put(64,7);unsigned endpoints[8]={10,105,20,70,90,8,30,120};
 for(unsigned v:endpoints)b.put(v,7);b.put(0,1);b.put(1,1);b.put(2,3);
 for(unsigned k=1;k<16;k++)b.put((k*7)%16,4);
 if(b.pos!=128)return 1;unsigned char image[80];std::memset(image,205,sizeof(image));bcdec_bc7(b.data,image,20);
 if(image[0]!=47||image[1]!=54||image[2]!=157||image[3]!=85)return 2;
 for(int y=0;y<4;y++)for(int x=16;x<20;x++)if(image[y*20+x]!=205)return 3;
 std::cout<<"BC7_MODE6_DECODE_OK first="<<int(image[0])<<","<<int(image[1])<<","<<int(image[2])<<","<<int(image[3])<<" stride=20\n";
}
