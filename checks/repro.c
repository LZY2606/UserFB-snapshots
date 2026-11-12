#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int deny;
static void* controlled_realloc(void*p,size_t n){return deny?NULL:realloc(p,n);}
#define TSF_MALLOC malloc
#define TSF_FREE free
#define TSF_REALLOC controlled_realloc
#define TSF_IMPLEMENTATION
#include "tsf.h"
#include "font_fixture.h"
int main(int argc,char**argv){
 if(argc!=2)return 3;tsf*f=make_font(2,7);if(!f)return 4;tsf_set_output(f,TSF_STEREO_INTERLEAVED,22050,0);
 if(!strcmp(argv[1],"layers")){int rc=tsf_note_on(f,0,60,1);int live=0,release=0;for(int i=0;i<f->voiceNum;i++)if(f->voices[i].playingPreset!=-1){live+=f->voices[i].ampenv.segment<TSF_SEGMENT_RELEASE;release+=f->voices[i].ampenv.segment==TSF_SEGMENT_RELEASE;}printf("LAYER_REPRO rc=%d live=%d release=%d expected_live=2\n",rc,live,release);tsf_close(f);return rc==1&&live==2&&release==0?0:2;}
 int before=f->voiceNum;deny=1;int rc=tsf_note_on(f,0,60,1);deny=0;int valid=rc==0&&f->voiceNum==before&&f->voices==NULL;printf("ALLOC_REPRO rc=%d voiceNum=%d before=%d pointer_null=%d\n",rc,f->voiceNum,before,f->voices==NULL);if(f->voices==NULL)f->voiceNum=0;tsf_close(f);return valid?0:2;
}
