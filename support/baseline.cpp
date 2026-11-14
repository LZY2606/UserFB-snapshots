#include "api/zimg.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(){
 zimg_image_format s,d;zimg_image_format_default(&s,ZIMG_API_VERSION);s.width=16;s.height=8;s.pixel_type=ZIMG_PIXEL_BYTE;s.depth=8;s.color_family=ZIMG_COLOR_GREY;s.pixel_range=ZIMG_RANGE_FULL;d=s;d.pixel_type=ZIMG_PIXEL_WORD;d.depth=16;
 zimg_graph_builder_params p;zimg_graph_builder_params_default(&p,ZIMG_API_VERSION);p.cpu_type=ZIMG_CPU_NONE;
 auto *g=zimg_filter_graph_build(&s,&d,&p);assert(g);size_t n;assert(!zimg_filter_graph_get_tmp_size(g,&n));void *tmp;assert(!posix_memalign(&tmp,64,n?n:64));
 alignas(64) unsigned char src[8][64]={};alignas(64) unsigned short dst[8][32]={};for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x)src[y][x]=(x+y)*7;
 zimg_image_buffer_const in={};zimg_image_buffer out={};in.version=out.version=ZIMG_API_VERSION;in.plane[0]={src,64,ZIMG_BUFFER_MAX};out.plane[0]={dst,64,ZIMG_BUFFER_MAX};
 assert(!zimg_filter_graph_process(g,&in,&out,tmp,nullptr,nullptr,nullptr,nullptr));for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x)assert(dst[y][x]==src[y][x]*257u);
 puts("ZIMG_DEPTH_GRAPH_OK pixels=128");zimg_filter_graph_free(g);free(tmp);
}
