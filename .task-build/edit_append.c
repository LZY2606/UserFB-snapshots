#include <gpac/isomedia.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 GF_ISOFile*m,*r; GF_TextSampleDescriptor d={0}; GF_FontRecord f={0}; GF_TextSample*t; GF_ISOSample*s; u32 tr,idx; GF_Err e;
 gf_sys_init(GF_MemTrackerNone,"n");
 remove(".task-build/edit_append.mp4");
 m=gf_isom_open(".task-build/edit_append.mp4",GF_ISOM_WRITE_EDIT,NULL);
 tr=gf_isom_new_track(m,7,GF_ISOM_MEDIA_TEXT,1000);
 d.tag=GF_ODF_TEXT_CFG_TAG; f.fontID=1;f.fontName="Serif"; d.font_count=1;d.fonts=&f;d.default_style.fontID=1;d.default_style.font_size=18;d.default_style.text_color=-1;
 gf_isom_new_text_description(m,tr,&d,0,0,&idx);
 t=gf_isom_new_text_sample(); gf_isom_text_add_text(t,"keep",4); s=gf_isom_text_to_sample(t); e=gf_isom_add_sample(m,tr,idx,s); fprintf(stderr,"add1 %d e=%d\n",gf_isom_get_sample_count(m,tr),e); gf_isom_sample_del(&s); gf_isom_delete_text_sample(t);
 gf_isom_set_last_sample_duration(m,tr,500);
 fprintf(stderr,"close1=%d\n",gf_isom_close(m));
 m=gf_isom_open(".task-build/edit_append.mp4",GF_ISOM_OPEN_READ_EDIT,NULL);
 fprintf(stderr,"opened tracks=%u samples=%u mode?\n",gf_isom_get_track_count(m),gf_isom_get_sample_count(m,1));
 t=gf_isom_new_text_sample(); gf_isom_text_add_text(t,"more",4); s=gf_isom_text_to_sample(t); s->DTS=500; e=gf_isom_add_sample(m,1,1,s); fprintf(stderr,"add2 count=%u e=%d\n",gf_isom_get_sample_count(m,1),e); gf_isom_sample_del(&s); gf_isom_delete_text_sample(t);
 e=gf_isom_set_last_sample_duration(m,1,700); fprintf(stderr,"setdur=%d\n",e);
 e=gf_isom_close(m); fprintf(stderr,"close2=%d\n",e);
 r=gf_isom_open(".task-build/edit_append.mp4",GF_ISOM_OPEN_READ,NULL);
 fprintf(stderr,"reopen samples=%u\n",gf_isom_get_sample_count(r,1));
 gf_isom_close(r); gf_sys_close(); return 0;
}
