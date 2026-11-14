#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void u16(FILE *f,unsigned x){fputc(x&255,f);fputc((x>>8)&255,f);}
static void u32(FILE *f,unsigned x){u16(f,x);u16(f,x>>16);}
static void block(FILE*f,int pred,int bytes){u16(f,(unsigned short)pred);u16(f,0);for(int i=4;i<bytes;i++)fputc(0x11,f);}
static void fixture(const char *path,int tail){
 FILE*f=fopen(path,"wb"); if(!f)exit(2);
 unsigned data=tail?13:8, pad=data&1;
 fwrite("RIFF",1,4,f);u32(f,4+28+8+data+pad+(tail?12:0));fwrite("WAVEfmt ",1,8,f);u32(f,20);
 u16(f,0x11);u16(f,1);u32(f,8000);u32(f,7111);u16(f,8);u16(f,4);u16(f,2);u16(f,9);
 fwrite("data",1,4,f);u32(f,data);block(f,1000,8);if(tail)block(f,-1000,5);if(pad)fputc(0,f);
 if(tail){fwrite("JUNK",1,4,f);u32(f,4);fwrite("zzzz",1,4,f);}fclose(f);
}
int main(int argc,char**argv){
 SF_INFO info={0}; short pcm[64]={0};fixture(".task-build/normal.wav",0);
 SNDFILE*f=sf_open(".task-build/normal.wav",SFM_READ,&info);if(!f){puts(sf_strerror(NULL));return 3;}
 sf_count_t n=sf_readf_short(f,pcm,64);if(info.frames!=9||n!=9)return 4;
 for(int i=0;i<9;i++)if(pcm[i]!=1000+i)return 5;
 if(sf_close(f))return 6;puts("NORMAL_OK frames=9 pcm=1000..1008");
 if(argc>1&&!strcmp(argv[1],"target"))return 0;
 fixture(".task-build/tail.wav",1);memset(&info,0,sizeof(info));memset(pcm,0,sizeof(pcm));
 f=sf_open(".task-build/tail.wav",SFM_READ,&info);if(!f){puts(sf_strerror(NULL));return 7;}
 n=sf_readf_short(f,pcm,64);
 printf("TAIL_OBSERVATION declared=%lld read=%lld physical_expected=12 tail=%d,%d,%d phantom=%d\n",(long long)info.frames,(long long)n,pcm[9],pcm[10],pcm[11],pcm[12]);
 if(sf_close(f))return 8;return 0;
}
