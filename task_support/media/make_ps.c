/* Original audio PES fixture builder, CC0-1.0. PTS bit layout and MPEG-1
 * pack prefix follow an earlier public scaffold at the same upstream. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
static void pts(FILE*f,uint64_t v){
 unsigned char b[5]={(unsigned char)(0x21|(((v>>30)&7)<<1)),(unsigned char)(v>>22),(unsigned char)((((v>>15)&127)<<1)|1),(unsigned char)(v>>7),(unsigned char)(((v&127)<<1)|1)};
 assert(fwrite(b,1,5,f)==5);
}
int main(int argc,char**argv){
 assert(argc==4);unsigned char bytes[2][8192];size_t n[2];
 for(int c=0;c<2;c++){FILE*f=fopen(argv[c+1],"rb");assert(f);n[c]=fread(bytes[c],1,sizeof(bytes[c]),f);assert(feof(f)&&n[c]>0&&n[c]%8==0);fclose(f);}
 FILE*f=fopen(argv[3],"wb");assert(f);
 unsigned char prefix[]={0,0,1,0xba,0x21,0,1,0,1,0xc3,0x33,0x67,0,0,1,0xbb,0,12,0xc3,0x33,0x67,8,0x20,0xff,0xc0,0xc0,0x20,0xc1,0xc0,0x20};
 assert(fwrite(prefix,1,sizeof(prefix),f)==sizeof(prefix));
 int pos[2]={0};uint64_t step[2]={3240,2160},offset[2]={0,1080};
 while(pos[0]<8||pos[1]<8){
  int c=pos[0]==8?1:(pos[1]==8?0:((offset[0]+step[0]*pos[0]<=offset[1]+step[1]*pos[1])?0:1));
  size_t sz=n[c]/8;unsigned char*p=bytes[c]+pos[c]*sz;assert(p[0]==255&&(p[1]&0xfe)==0xfc);
  unsigned char h[]={0,0,1,(unsigned char)(0xc0+c),(unsigned char)((sz+5)>>8),(unsigned char)(sz+5)};
  assert(fwrite(h,1,6,f)==6);pts(f,90000+offset[c]+step[c]*pos[c]);assert(fwrite(p,1,sz,f)==sz);pos[c]++;
 }
 unsigned char end[]={0,0,1,0xb9};assert(fwrite(end,1,4,f)==4);return fclose(f)!=0;
}
