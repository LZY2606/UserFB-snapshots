#include "src/cmixer.h"
#include <stdio.h>
#include <stdlib.h>
struct stream {int pos, reads, rewinds;};
static void handler(cm_Event *e) {
 struct stream *s=e->udata;
 if(e->type==CM_EVENT_REWIND){s->pos=0;s->rewinds++;}
 if(e->type==CM_EVENT_SAMPLES){for(int i=0;i<e->length;i+=2){int p=s->pos++%1024;e->buffer[i]=1000+p%97;e->buffer[i+1]=-2000+p%47;}s->reads++;}
}
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"baseline:%d %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 struct stream s={0};cm_Int16 out[1200];
 cm_init(48000);cm_SourceInfo info={handler,&s,48000,1024};cm_Source *src=cm_new_source(&info);REQUIRE(src);cm_play(src);
 cm_process(out,1200);
 for(int i=0;i<600;i++){REQUIRE(out[i*2]==1000+i%97);REQUIRE(out[i*2+1]==-2000+i%47);}
 REQUIRE(s.reads>=4);REQUIRE(s.rewinds==1);REQUIRE(cm_get_position(src)==600.0/48000);
 cm_pause(src);cm_process(out,32);for(int i=0;i<32;i++)REQUIRE(out[i]==0);
 cm_play(src);cm_process(out,200);for(int i=0;i<100;i++){REQUIRE(out[i*2]==1000+(600+i)%97);REQUIRE(out[i*2+1]==-2000+(600+i)%47);}
 cm_destroy_source(src);puts("CMIXER_BASELINE_OK frames=700 stereo=1 pause_resume=1");return 0;
}
