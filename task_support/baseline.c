#include <framework/mlt.h>
#include <stdio.h>
#include <stdlib.h>
static int source_frame(mlt_producer p,mlt_frame *f,int index){
 (void)index;*f=mlt_frame_init(MLT_PRODUCER_SERVICE(p));
 mlt_properties_set_int(MLT_FRAME_PROPERTIES(*f),"source_frame",mlt_producer_position(p));
 mlt_producer_prepare_next(p);return *f?0:1;
}
static int read_at(mlt_producer p,int pos){
 mlt_frame f=NULL;mlt_producer_seek(p,pos);
 if(mlt_service_get_frame(MLT_PRODUCER_SERVICE(p),&f,0)||!f)exit(3);
 int n=mlt_properties_get_int(MLT_FRAME_PROPERTIES(f),"source_frame");mlt_frame_close(f);return n;
}
int main(void){
 struct mlt_profile_s profile={.frame_rate_num=25,.frame_rate_den=1,.width=32,.height=32,.progressive=1,.sample_aspect_num=1,.sample_aspect_den=1,.display_aspect_num=1,.display_aspect_den=1,.colorspace=709,.is_explicit=1};
 mlt_pool_init();
 mlt_producer source=mlt_producer_new(&profile);source->get_frame=source_frame;
 mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(source),"length",10);
 mlt_producer_set_in_and_out(source,0,9);
 mlt_producer cut=mlt_producer_cut(source,2,-1);
 mlt_playlist pl=mlt_playlist_new(&profile);
 if(mlt_playlist_append(pl,cut)||mlt_playlist_repeat_clip(pl,0,2))return 2;
 if(mlt_producer_get_playtime(cut)!=8||mlt_playlist_clip_length(pl,0)!=16||read_at(MLT_PLAYLIST_PRODUCER(pl),8)!=2)return 4;
 printf("STATIC_CUT_OK range=2..9 repeat_length=16 source_frame=2\n");
 mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(source),"length",20);
 mlt_producer_set_in_and_out(source,0,19);
 printf("AFTER_PARENT_GROW source=%d cut_length=%d cut_out=%d playlist=%d\n",(int)mlt_producer_get_length(source),(int)mlt_producer_get_length(cut),(int)mlt_producer_get_out(cut),(int)mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(pl)));
 mlt_producer_clear(cut);
 printf("CLEARED_CUT playtime=%d playlist=%d\n",(int)mlt_producer_get_playtime(cut),(int)mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(pl)));
 mlt_playlist_close(pl);mlt_producer_close(cut);mlt_producer_close(source);mlt_pool_close();
 return 0;
}
