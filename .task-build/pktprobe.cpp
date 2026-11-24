#include "rtppacketbuilder.h"
#include <cstdio>
#include <cstring>
int main(){
 unsigned char rtp[]={0x80,0x60,0,1,0,0,0,1,1,2,3,4};
 unsigned char rr[]={0x80,201,0,1,0,0,0,42};
 // classification: version check and second byte rule
 for(int i=0;i<2;i++){
  unsigned char *d=i?rr:rtp; int len=i?8:12;
  bool isrtp = !(d[1]>=192 && d[1]<=223);
  printf("i=%d ver=%d b1=%d isrtp=%d\n",i,(d[0]>>6)&3,d[1],isrtp);
 }
 unsigned char marker[]={0x80,0xE0,0,1,0,0,0,1,0,0,0,2,'a','b','c','d'};
 printf("marker b1=%d isrtp=%d\n",marker[1],!(marker[1]>=192&&marker[1]<=223));
}
