#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sndfile.h"
#include "src/ALAC/alac_codec.h"
#include "src/ALAC/ALACBitUtilities.h"
static void be(FILE*f,uint64_t n,int bytes){for(int i=bytes-1;i>=0;i--)fputc((int)(n>>(8*i))&255,f);}
static void chunk(FILE*f,const char*s,uint64_t n){fwrite(s,1,4,f);be(f,n,8);}
static unsigned vint(unsigned char *p,uint32_t x){unsigned char b[5];unsigned n=0,k=0;do{b[n++]=x&127;x>>=7;}while(x);while(n){n--;p[k++]=b[n]|(n?128:0);}return k;}
static int32_t pcm(unsigned i){return ((int32_t)((i*149+31)%12001)-6000)*65536;}
static int normal(void){
 SF_INFO info={0};info.samplerate=48000;info.channels=1;info.format=SF_FORMAT_CAF|SF_FORMAT_ALAC_16;
 SNDFILE*f=sf_open(".task-build/normal.caf",SFM_WRITE,&info);if(!f)return 2;
 int in[1409],out[1409];for(unsigned i=0;i<1409;i++)in[i]=pcm(i);
 if(sf_writef_int(f,in,1409)!=1409||sf_close(f))return 3;
 memset(&info,0,sizeof(info));f=sf_open(".task-build/normal.caf",SFM_READ,&info);if(!f)return 4;
 if(sf_readf_int(f,out,1409)!=1409||memcmp(in,out,sizeof(in)))return 5;
 printf("CAF_ALAC_NORMAL frames=%lld pcm=equal\n",(long long)info.frames);sf_close(f);return 0;
}
static int variable(void){
 const uint32_t counts[3]={127,1025,257};
 unsigned char packets[3][20000],table[40],cookie[128];uint32_t sizes[3],nc=sizeof(cookie);unsigned nt=0,total=0;
 ALAC_ENCODER*enc=calloc(1,sizeof(*enc));ALAC_DECODER*dec=calloc(1,sizeof(*dec));if(!enc||!dec)return 6;
 if(alac_encoder_init(enc,48000,1,1,4096))return 7;
 for(unsigned p=0;p<3;p++){
  int32_t in[4096];for(unsigned i=0;i<counts[p];i++)in[i]=pcm(total+i);
  sizes[p]=sizeof(packets[p]);if(alac_encode(enc,counts[p],in,packets[p],&sizes[p]))return 8;
  nt+=vint(table+nt,sizes[p]);nt+=vint(table+nt,counts[p]);total+=counts[p];
 }
 alac_get_magic_cookie(enc,cookie,&nc);if(!nc||alac_decoder_init(dec,cookie,nc))return 9;
 unsigned pos=0;
 for(unsigned p=0;p<3;p++){
  int32_t out[4096];uint32_t got=0;BitBuffer bits;BitBufferInit(&bits,packets[p],sizes[p]);
  if(alac_decode(dec,&bits,out,4096,&got)||got!=counts[p])return 10;
  for(unsigned i=0;i<got;i++)if(out[i]!=pcm(pos+i))return 11;
  pos+=got;
 }
 printf("ALAC_PACKETS_DIRECT packets=3 frames=%u pcm=equal\n",total);
 FILE*f=fopen(".task-build/variable.caf","wb");if(!f)return 12;
 fwrite("caff",1,4,f);be(f,1,2);be(f,0,2);
 chunk(f,"desc",32);be(f,UINT64_C(0x40e7700000000000),8);fwrite("alac",1,4,f);be(f,1,4);be(f,0,4);be(f,0,4);be(f,1,4);be(f,0,4);
 chunk(f,"kuki",nc);fwrite(cookie,1,nc,f);
 chunk(f,"pakt",24+nt);be(f,3,8);be(f,total,8);be(f,0,4);be(f,0,4);fwrite(table,1,nt,f);
 uint64_t data=4;for(int p=0;p<3;p++)data+=sizes[p];chunk(f,"data",data);be(f,0,4);
 for(int p=0;p<3;p++)fwrite(packets[p],1,sizes[p],f);if(fclose(f))return 13;
 SF_INFO info={0};SNDFILE*s=sf_open(".task-build/variable.caf",SFM_READ,&info);
 if(!s)printf("CAF_VPACKET_CURRENT open=failed wanted_frames=%u error=%s\n",total,sf_strerror(NULL));
 else{printf("CAF_VPACKET_CURRENT frames=%lld wanted_frames=%u\n",(long long)info.frames,total);sf_close(s);}
 free(enc);free(dec);return 0;
}
int main(void){int r=normal();if(r)return r;return variable();}
