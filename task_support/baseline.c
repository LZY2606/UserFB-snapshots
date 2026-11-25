#include <gpac/internal/isomedia_dev.h>
#include <stdio.h>
#include <string.h>
#define NEED(x) do { if (!(x)) {fprintf(stderr,"baseline failed at %d: %s\n",__LINE__,#x);return 2;} } while(0)
int main(void) {
 GF_ISOFile *m; GF_TextSample *tx,*parsed; GF_ISOSample *s; GF_BitStream *bs;
 GF_TextSampleDescriptor desc={0}, *read_desc=NULL;GF_FontRecord font={0};GF_StyleRecord style={0};
 u32 tr,idx,got_idx,i,kroks=0;GF_Box *box;GF_Err e;
 NEED(gf_sys_init(GF_MemTrackerNone,"n")==GF_OK);
 m=gf_isom_open(".task-build/source.mp4",GF_ISOM_WRITE_EDIT,NULL);NEED(m);
 tr=gf_isom_new_track(m,1,GF_ISOM_MEDIA_TEXT,1000);NEED(tr==1);
 desc.tag=GF_ODF_TEXT_CFG_TAG;font.fontID=1;font.fontName="Serif";
 desc.font_count=1;desc.fonts=&font;desc.default_style.fontID=1;
 desc.default_style.font_size=18;desc.default_style.text_color=0xFFFFFFFF;
 NEED(gf_isom_new_text_description(m,tr,&desc,NULL,NULL,&idx)==GF_OK);
 tx=gf_isom_new_text_sample();NEED(tx);NEED(gf_isom_text_add_text(tx,"AB",2)==GF_OK);
 style=desc.default_style;style.startCharOffset=1;style.endCharOffset=2;style.style_flags=1;
 NEED(gf_isom_text_add_style(tx,&style)==GF_OK);
 NEED(gf_isom_text_set_highlight_color(tx,0xFFFF0000)==GF_OK);
 NEED(gf_isom_text_add_karaoke(tx,100)==GF_OK);
 NEED(gf_isom_text_set_karaoke_segment(tx,400,0,1)==GF_OK);
 NEED(gf_isom_text_set_karaoke_segment(tx,800,1,2)==GF_OK);
 NEED(gf_isom_text_add_karaoke(tx,200)==GF_OK);
 NEED(gf_isom_text_set_karaoke_segment(tx,600,0,2)==GF_OK);
 s=gf_isom_text_to_sample(tx);NEED(s);s->DTS=0;s->IsRAP=RAP;
 NEED(gf_isom_add_sample(m,tr,idx,s)==GF_OK);
 NEED(gf_isom_set_last_sample_duration(m,tr,1000)==GF_OK);
 gf_isom_sample_del(&s);gf_isom_delete_text_sample(tx);
 NEED(gf_isom_close(m)==GF_OK);
 m=gf_isom_open(".task-build/source.mp4",GF_ISOM_OPEN_READ,NULL);NEED(m);
 NEED(gf_isom_get_sample_count(m,1)==1);NEED(gf_isom_get_sample_duration(m,1,1)==1000);
 NEED(gf_isom_get_media_timescale(m,1)==1000);
 s=gf_isom_get_sample(m,1,1,&got_idx);NEED(s&&got_idx==1);
 bs=gf_bs_new(s->data,s->dataLength,GF_BITSTREAM_READ);NEED(bs);
 parsed=gf_isom_parse_text_sample(bs);NEED(parsed&&parsed->len==2&&!memcmp(parsed->text,"AB",2));
 gf_bs_del(bs);NEED(parsed->styles&&parsed->styles->entry_count==1);
 i=0;while((box=gf_list_enum(parsed->others,&i)))if(box->type==GF_ISOM_BOX_TYPE_KROK)kroks++;
 NEED(kroks==2&&parsed->cur_karaoke&&parsed->cur_karaoke->highlight_starttime==200);
 NEED(gf_isom_get_text_description(m,1,1,&read_desc)==GF_OK&&read_desc&&read_desc->font_count==1);
 printf("GPAC_TX3G_BASELINE_OK samples=%u duration=%u karaoke_boxes=%u font=%s\n",gf_isom_get_sample_count(m,1),gf_isom_get_sample_duration(m,1,1),kroks,read_desc->fonts[0].fontName);
 gf_odf_desc_del((GF_Descriptor*)read_desc);gf_isom_delete_text_sample(parsed);gf_isom_sample_del(&s);
 e=gf_isom_close(m);NEED(e==GF_OK);gf_sys_close();return 0;
}
