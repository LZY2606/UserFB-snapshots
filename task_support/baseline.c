#include "task_support/fixture.h"
#include <stdlib.h>
int main(void){task_memory ordinary,segmented;SF_INFO info;SNDFILE* f;short pcm[64]={0},part[6]={0};sf_count_t got;unsigned i;
 task_ima(&ordinary,0);f=task_open(&ordinary,&info);if(!f){fprintf(stderr,"ordinary open: %s\n",sf_strerror(NULL));return 1;}
 got=sf_readf_short(f,pcm,16);if(got!=9||info.channels!=2||info.frames!=9)return 2;
 for(i=0;i<9;++i)if(pcm[2*i]!=(short)(1000+i)||pcm[2*i+1]!=(short)(-1000-(int)i))return 3;
 if(sf_seek(f,4,SEEK_SET)!=4||sf_readf_short(f,part,3)!=3)return 4;
 if(memcmp(part,pcm+8,sizeof(part)))return 5;
 if(sf_close(f))return 6;
 printf("IMA_PRODUCTION_OK frames=%lld first=%d,%d tail=%d,%d seek=4\n",(long long)got,pcm[0],pcm[1],pcm[16],pcm[17]);
 task_ima(&segmented,1);f=task_open(&segmented,&info);
 if(f){got=sf_readf_short(f,pcm,32);printf("WAVL_STARTING_OBSERVATION opened=1 declared_frames=%lld read_frames=%lld desired_frames=23\n",(long long)info.frames,(long long)got);if(sf_close(f))return 7;}
 else printf("WAVL_STARTING_OBSERVATION opened=0 error=%s desired_frames=23\n",sf_strerror(NULL));
 puts("IMA_BASELINE_OK normal_decode_and_seek=1");return 0;}
