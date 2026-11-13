#include "smacker.h"
#include <vector>
#include <cassert>
#include <cstdio>
using V=std::vector<unsigned char>;
struct Bits{V v;unsigned n=0;void put(int x){if(n%8==0)v.push_back(0);v.back()|=(x&1)<<(n++%8);}void val(unsigned x,int k){for(int i=0;i<k;i++)put(x>>i);}};
void u32(V&v,unsigned x){for(int i=0;i<4;i++)v.push_back(x>>(8*i));}
int main(){Bits b;for(int i=0;i<3;i++){b.put(0);b.put(0);}b.put(1);
 for(unsigned x:{3u,7u}){b.put(1);b.put(0);b.val(x,8);b.put(0);}
 for(int i=0;i<3;i++)b.val(65535,16);b.put(0);b.put(0);
 V data={'S','M','K','4'};for(unsigned x:{4u,4u,1u,100u,0u})u32(data,x);
 for(int i=0;i<7;i++)u32(data,0);u32(data,b.v.size());for(unsigned x:{0u,0u,0u,16u})u32(data,x);
 for(int i=0;i<8;i++)u32(data,0);u32(data,4);data.push_back(0);data.insert(data.end(),b.v.begin(),b.v.end());data.insert(data.end(),4,0);
 smk s=smk_open_memory(data.data(),data.size());assert(s);assert(smk_enable_video(s,1)==0);assert(smk_first(s)==SMK_LAST);
 const unsigned char* pixels=smk_get_video(s);for(int i=0;i<16;i++)assert(pixels[i]==7);assert(smk_next(s)==SMK_DONE);smk_close(s);
 printf("BASELINE_SMACKER_PASS frames=1 pixels=16 color=7\n");}
