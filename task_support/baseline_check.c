#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"baseline assertion failed: %s line=%d\n",#x,__LINE__);return 1;} } while(0)
int main(int argc,char**argv){
 CHECK(argc==3);FILE*v=fopen(argv[1],"rb"),*a=fopen(argv[2],"rb");CHECK(v&&a);
 CHECK(!fseek(v,0,SEEK_END));long vs=ftell(v);CHECK(vs==45*64);
 CHECK(!fseek(v,0,SEEK_SET));int first=fgetc(v);CHECK(first==0);
 CHECK(!fseek(v,22*64,SEEK_SET));int mid=fgetc(v);CHECK(abs(mid-102)<=1);
 CHECK(!fseek(v,44*64,SEEK_SET));int tail=fgetc(v);CHECK(tail==255);
 CHECK(!fseek(a,0,SEEK_END));long as=ftell(a);CHECK(as==86400L*2*4);
 CHECK(!fseek(a,0,SEEK_SET));float p[2];CHECK(fread(p,sizeof(float),2,a)==2);
 CHECK(fabsf(p[0]-.25f)<1e-6f&&fabsf(p[1]+.125f)<1e-6f);
 CHECK(!fseek(a,43200L*2*4,SEEK_SET));CHECK(fread(p,sizeof(float),2,a)==2);
 CHECK(fabs(p[0]-(.25*(4799.0/9600)-.5*.5))<1e-6);
 CHECK(fabs(p[1]-(-.125*(4799.0/9600)+.375*.5))<1e-6);
 CHECK(!fseek(a,86399L*2*4,SEEK_SET));CHECK(fread(p,sizeof(float),2,a)==2);
 CHECK(fabsf(p[0]+.5f)<1e-6f&&fabsf(p[1]-.375f)<1e-6f);
 fclose(v);fclose(a);
 printf("FFMPEG_TRANSITION_BASELINE_OK video_frames=45 midpoint=%d audio_frames=86400 stereo=ok tail=ok\n",mid);
 return 0;
}
