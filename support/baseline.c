#include <framework/mlt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
extern mlt_transition transition_mix_init(mlt_profile,mlt_service_type,const char*,char*);
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 mlt_pool_init();
 mlt_profile profile=mlt_profile_init(NULL);CHECK(profile);profile->frame_rate_num=25;profile->frame_rate_den=1;
 mlt_transition t=transition_mix_init(profile,mlt_service_transition_type,"mix","0.25");CHECK(t);
 mlt_properties producer=mlt_properties_new();mlt_properties_set_int(producer,"in",0);mlt_properties_set_int(producer,"out",10);
 mlt_frame a=mlt_frame_init(NULL),b=mlt_frame_init(NULL);
 float *aa=calloc(8,sizeof(float)),*bb=calloc(8,sizeof(float));for(int i=0;i<8;i++)bb[i]=1;
 mlt_frame_set_audio(a,aa,mlt_audio_f32le,8*sizeof(float),free);mlt_frame_set_audio(b,bb,mlt_audio_f32le,8*sizeof(float),free);
 for(int k=0;k<2;k++){mlt_properties fp=MLT_FRAME_PROPERTIES(k?b:a);mlt_properties_set_int(fp,"audio_frequency",48000);mlt_properties_set_int(fp,"audio_channels",1);mlt_properties_set_int(fp,"audio_samples",8);mlt_properties_set_int(fp,"test_audio",0);}
 mlt_properties_set_data(MLT_FRAME_PROPERTIES(b),"_producer",producer,0,NULL,NULL);
 mlt_properties_set_int(MLT_FRAME_PROPERTIES(b),"_speed",1);
 mlt_frame_set_position(a,0);mlt_frame_set_position(b,0);
 t->process(t,a,b);void *out=NULL;mlt_audio_format format=mlt_audio_f32le;int rate=48000,channels=1,samples=8;
 CHECK(!mlt_frame_get_audio(a,&out,&format,&rate,&channels,&samples));CHECK(out&&samples==8);CHECK(fabs(((float*)out)[0]-0.25)<1e-6);
 mlt_properties p=mlt_properties_new();mlt_properties_set(p,"curve","0=0;10=1");double v=mlt_properties_anim_get_double(p,"curve",5,11);CHECK(fabs(v-0.5)<1e-12);
 printf("baseline mixed=%g samples=%d integer-curve=%g\n",((float*)out)[0],samples,v);
 mlt_frame_close(a);mlt_frame_close(b);mlt_transition_close(t);mlt_properties_close(producer);mlt_properties_close(p);mlt_profile_close(profile);return 0;
}
