#include <libchdr/chd.h>
#include <libchdr/cdrom.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void be32(unsigned char*p,unsigned v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static unsigned edc(const unsigned char*p,unsigned n){unsigned r=0,i,b;for(i=0;i<n;i++){r^=p[i];for(b=0;b<8;b++)r=(r>>1)^((r&1)?0xd8018001u:0);}return r;}
int main(void){unsigned char image[4896]={0},out[2448],sector[2352]={0};chd_file*c=0;unsigned i;
 sector[0]=0;memset(sector+1,255,10);sector[15]=1;for(i=16;i<2064;i++)sector[i]=(unsigned char)(i*7);unsigned e=edc(sector,2064);for(i=0;i<4;i++)sector[2064+i]=(unsigned char)(e>>(i*8));ecc_generate(sector);assert(ecc_verify(sector));sector[100]^=83;assert(!ecc_verify(sector));puts("CD_ECC_BASELINE_OK damaged=detected");
 memcpy(image,"MComprHD",8);be32(image+8,124);be32(image+12,5);be32(image+36,2448);be32(image+44,124);be32(image+56,2448);be32(image+60,2448);be32(image+124,1);memcpy(image+2448,sector,2352);memset(image+2448+2352,0xa5,96);
 FILE*f=tmpfile();assert(f);assert(fwrite(image,1,sizeof(image),f)==sizeof(image));fflush(f);assert(chd_open_file(f,CHD_OPEN_READ,0,&c)==CHDERR_NONE);assert(chd_read(c,0,out)==CHDERR_NONE);assert(!memcmp(out,sector,2352));for(i=2352;i<2448;i++)assert(out[i]==0xa5);chd_close(c);fclose(f);puts("CHD_RAW_HUNK_OK bytes=2448 subcode=96");return 0;}
