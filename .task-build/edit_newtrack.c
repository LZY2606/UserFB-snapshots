#include <gpac/isomedia.h>
#include <stdio.h>
int main(void) {
 GF_ISOFile *src,*dst,*chk; u32 out=0; GF_Err e;
 gf_sys_init(GF_MemTrackerNone,"n");
 src=gf_isom_open(".task-build/karaoke_event.mp4",GF_ISOM_OPEN_READ,NULL);
 dst=gf_isom_open(".task-build/karaoke_fail_dst.mp4",GF_ISOM_OPEN_EDIT,NULL);
 fprintf(stderr,"src=%p dst=%p tracks_before=%u\n",(void*)src,(void*)dst,dst?gf_isom_get_track_count(dst):0);
 e=gf_isom_text_bake_karaoke(src,1,dst,&out);
 fprintf(stderr,"bake e=%d out=%u tracks_after=%u\n",e,out,gf_isom_get_track_count(dst));
 e=gf_isom_close(dst); fprintf(stderr,"close=%d\n",e);
 gf_isom_close(src);
 chk=gf_isom_open(".task-build/karaoke_fail_dst.mp4",GF_ISOM_OPEN_READ,NULL);
 fprintf(stderr,"reopen tracks=%u\n",gf_isom_get_track_count(chk));
 if (out) fprintf(stderr,"baked samples=%u dur=%llu\n",gf_isom_get_sample_count(chk,out),(unsigned long long)gf_isom_get_media_duration(chk,out));
 gf_isom_close(chk); gf_sys_close(); return 0;
}
