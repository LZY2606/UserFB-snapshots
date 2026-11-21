#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define N 10000
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"baseline check failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static uint64_t be64(const unsigned char *p){uint64_t n=0;int i;for(i=0;i<8;i++)n=(n<<8)|p[i];return n;}
static void put(unsigned char *p,uint64_t n,int bytes){int i;for(i=bytes-1;i>=0;i--){p[i]=(unsigned char)n;n>>=8;}}
int main(void){
 const char *name=".task-build/baseline.caf";
 SF_INFO info={0}; SNDFILE *f; short pcm[N*2],got[N*2]; int i; sf_count_t n;
 FILE *raw; unsigned char header[12],fields[24]; long pos; int found=0;
 for(i=0;i<N;i++){pcm[2*i]=(short)((i*17)%24001-12000);pcm[2*i+1]=(short)(9000-(i*23)%18001);}
 info.samplerate=48000;info.channels=2;info.format=SF_FORMAT_CAF|SF_FORMAT_ALAC_16;
 f=sf_open(name,SFM_WRITE,&info);REQUIRE(f);REQUIRE(sf_writef_short(f,pcm,N)==N);REQUIRE(sf_close(f)==0);
 memset(&info,0,sizeof info);f=sf_open(name,SFM_READ,&info);REQUIRE(f);REQUIRE(info.frames==N);REQUIRE(info.channels==2);
 REQUIRE(sf_readf_short(f,got,N)==N);REQUIRE(memcmp(pcm,got,sizeof pcm)==0);
 REQUIRE(sf_seek(f,4099,SEEK_SET)==4099);REQUIRE(sf_readf_short(f,got,4)==4);REQUIRE(memcmp(pcm+8198,got,8*sizeof(short))==0);
 REQUIRE(sf_seek(f,-3,SEEK_END)==N-3);REQUIRE(sf_readf_short(f,got,4)==3);REQUIRE(memcmp(pcm+2*(N-3),got,6*sizeof(short))==0);REQUIRE(sf_close(f)==0);
 puts("CAF_ALAC_ROUNDTRIP_OK frames=10000 channels=2 packet_seek=4099 short_tail=3");
 raw=fopen(name,"r+b");REQUIRE(raw);REQUIRE(fseek(raw,8,SEEK_SET)==0);
 while(fread(header,1,12,raw)==12){uint64_t size=be64(header+4);pos=ftell(raw);REQUIRE(size<1000000);
  if(!memcmp(header,"pakt",4)){REQUIRE(size>=24);REQUIRE(fread(fields,1,24,raw)==24);REQUIRE(be64(fields)==3);put(fields+8,4000,8);put(fields+16,4300,4);put(fields+20,1700,4);REQUIRE(fseek(raw,pos,SEEK_SET)==0);REQUIRE(fwrite(fields,1,24,raw)==24);found=1;break;}
  REQUIRE(fseek(raw,(long)size,SEEK_CUR)==0);
 }
 REQUIRE(found);REQUIRE(fclose(raw)==0);memset(&info,0,sizeof info);f=sf_open(name,SFM_READ,&info);REQUIRE(f);
 n=sf_readf_short(f,got,N);REQUIRE(n>0);printf("CAF_WINDOW_PROBE declared_valid=4000 priming=4300 remainder=1700 reported=%lld read=%lld first=(%d,%d) expected_first=(%d,%d)\n",(long long)info.frames,(long long)n,got[0],got[1],pcm[8600],pcm[8601]);
 REQUIRE(sf_close(f)==0);REQUIRE(remove(name)==0);return 0;
}
