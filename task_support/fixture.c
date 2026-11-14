#include "fixture.h"
static int frame_alloc(h261_frame_t *f,int w,int h){
 f->i_width=f->i_stride_y=w;f->i_height=h;f->i_stride_c=w/2;
 f->pui8_Y=calloc((size_t)w*h,1);f->pui8_Cb=calloc((size_t)w*h/4,1);f->pui8_Cr=calloc((size_t)w*h/4,1);
 return f->pui8_Y&&f->pui8_Cb&&f->pui8_Cr;
}
int fixture_init(h261_context_t *c,int format,int mode){
 int w=format==H261_SOURCE_FORMAT_CIF?352:176,h=format==H261_SOURCE_FORMAT_CIF?288:144;
 memset(c,0,sizeof(*c));c->i_source_format=format;
 if(!frame_alloc(&c->s_current_frame,w,h)||!frame_alloc(&c->s_reconstructed_frame,w,h)||!frame_alloc(&c->s_reference_frame,w,h))return 0;
 c->ps_mb_cache=calloc((size_t)(w/16)*(h/16),sizeof(*c->ps_mb_cache));if(!c->ps_mb_cache)return 0;
 h261_bitstream_init(&c->s_bitstream,0x100000);
 h261_initialize_pixelop_functions();h261_initialize_transform_functions();
 h261_init_ratectrl(c,10,64000,64000,mode);return 1;
}
void fixture_fill(h261_context_t *c,int frame){
 h261_frame_t *f=&c->s_current_frame;
 for(int y=0;y<f->i_height;y++)for(int x=0;x<f->i_width;x++)f->pui8_Y[y*f->i_stride_y+x]=(UInt8)(32+((x+frame*3)/8*13+y/8*7)%192);
 for(int y=0;y<f->i_height/2;y++)for(int x=0;x<f->i_width/2;x++){
  f->pui8_Cb[y*f->i_stride_c+x]=(UInt8)(96+(x/8+frame)%48);
  f->pui8_Cr[y*f->i_stride_c+x]=(UInt8)(112+(y/8+frame)%40);
 }
}
unsigned fixture_encode(h261_context_t *c,int frame,FILE *out){
 h261_picture_parameters_t p;UInt8 *bytes;UInt32 n;
 p.i_frame_num=frame;p.i_source_format=c->i_source_format;p.i_frame_type=frame?H261_FRAME_TYPE_INTER:H261_FRAME_TYPE_INTRA;p.i_quantiser=10;
 c->i_frame_counter=frame;
 h261_ratectrl_init_picture(c,&p);h261_init_picture(c,&p);
 int before=h261_bitstream_bits(&c->s_bitstream);
 h261_write_picture_header(c);h261_encode_gobs(c);
 unsigned bits=(unsigned)(h261_bitstream_bits(&c->s_bitstream)-before);
 h261_bitstream_get(&c->s_bitstream,&bytes,&n);if(out)fwrite(bytes,1,n,out);
 h261_bitstream_advance(&c->s_bitstream);h261_ratectrl_update_picture(c,n);
 h261_frame_t *r=&c->s_reconstructed_frame,*ref=&c->s_reference_frame;
 memcpy(ref->pui8_Y,r->pui8_Y,(size_t)r->i_width*r->i_height);
 memcpy(ref->pui8_Cb,r->pui8_Cb,(size_t)r->i_width*r->i_height/4);
 memcpy(ref->pui8_Cr,r->pui8_Cr,(size_t)r->i_width*r->i_height/4);
 return bits;
}
void fixture_free(h261_context_t *c){
 h261_frame_t *frames[]={&c->s_current_frame,&c->s_reference_frame,&c->s_reconstructed_frame};
 for(int i=0;i<3;i++){free(frames[i]->pui8_Y);free(frames[i]->pui8_Cb);free(frames[i]->pui8_Cr);}
 free(c->ps_mb_cache);h261_bitstream_deinit(&c->s_bitstream);
}
