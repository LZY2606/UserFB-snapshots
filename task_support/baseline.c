#include <sndfile.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"baseline failure line %d\n",__LINE__);exit(1);}}while(0)
static void be(FILE*f,uint64_t x,int n){for(int i=n-1;i>=0;--i)fputc((unsigned)(x>>(i*8))&255,f);}
static void chunk(FILE*f,const char*s,uint64_t n){fwrite(s,1,4,f);be(f,n,8);}
int main(void){
 SF_INFO i={0};i.samplerate=8000;i.channels=3;i.format=SF_FORMAT_CAF|SF_FORMAT_PCM_16;
 int map[3]={SF_CHANNEL_MAP_RIGHT,SF_CHANNEL_MAP_LEFT,SF_CHANNEL_MAP_CENTER};short pcm[6]={10,20,30,-40,50,-60},out[6]={0};
 SNDFILE*s=sf_open("normal.caf",SFM_WRITE,&i);CHECK(s);int set=sf_command(s,SFC_SET_CHANNEL_MAP_INFO,map,sizeof map);CHECK(sf_writef_short(s,pcm,2)==2);CHECK(sf_close(s)==0);
 memset(&i,0,sizeof i);s=sf_open("normal.caf",SFM_READ,&i);CHECK(s);CHECK(sf_readf_short(s,out,2)==2);CHECK(memcmp(pcm,out,sizeof pcm)==0);CHECK(sf_close(s)==0);
 FILE*f=fopen("described.caf","wb");CHECK(f);fwrite("caff",1,4,f);be(f,1,2);be(f,0,2);chunk(f,"desc",32);double rate=8000;uint64_t bits;memcpy(&bits,&rate,8);be(f,bits,8);fwrite("lpcm",1,4,f);be(f,0,4);be(f,6,4);be(f,1,4);be(f,3,4);be(f,16,4);
 chunk(f,"chan",72);be(f,0,4);be(f,0,4);be(f,3,4);int labels[3]={2,1,3};for(int k=0;k<3;k++){be(f,labels[k],4);for(int j=0;j<4;j++)be(f,0,4);}chunk(f,"data",16);be(f,0,4);for(int k=0;k<6;k++)be(f,(uint16_t)pcm[k],2);CHECK(fclose(f)==0);
 memset(&i,0,sizeof i);s=sf_open("described.caf",SFM_READ,&i);if(!s)fprintf(stderr,"%s\n",sf_strerror(NULL));CHECK(s);memset(out,0,sizeof out);CHECK(sf_readf_short(s,out,2)==2);CHECK(memcmp(pcm,out,sizeof pcm)==0);int got=sf_command(s,SFC_GET_CHANNEL_MAP_INFO,map,sizeof map);CHECK(sf_close(s)==0);
 printf("CAF PCM frames=2 channels=3 preserved; custom_set=%d description_get=%d\n",set,got);remove("normal.caf");remove("described.caf");return 0;
}
