#ifndef TASK_GBS_FIXTURE_H
#define TASK_GBS_FIXTURE_H
#include <vector>
#include <cstddef>
static std::vector<unsigned char> task_gbs(unsigned char nr50) {
 std::vector<unsigned char> b(112+256,0);
 b[0]='G';b[1]='B';b[2]='S';b[3]=1;b[4]=1;b[5]=1;
 b[6]=0;b[7]=0x40;b[8]=0;b[9]=0x40;b[10]=0x80;b[11]=0x40;b[12]=0xfe;b[13]=0xff;
 const unsigned char pairs[]={0x26,0x80,0x24,nr50,0x25,0x11,0x11,0x80,0x12,0xf0,0x13,0,0x14,0x87};
 size_t p=112;
 for(size_t i=0;i<sizeof pairs;i+=2){b[p++]=0x3e;b[p++]=pairs[i+1];b[p++]=0xe0;b[p++]=pairs[i];}
 b[p]=0xc9;b[112+0x80]=0xc9;
 return b;
}
#endif
