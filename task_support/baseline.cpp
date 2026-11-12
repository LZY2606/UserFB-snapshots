#include "libimagequant.h"
#include <iostream>
int main(){
 liq_color px[8]; for(int k=0;k<8;k++)px[k]=k%2?liq_color{0,0,255,255}:liq_color{255,0,0,255};
 liq_attr*a=liq_attr_create(); if(liq_set_max_colors(a,2)!=LIQ_OK)return 1;
 liq_image*i=liq_image_create_rgba(a,px,4,2,0.45455);liq_result*r=nullptr;
 if(liq_image_quantize(i,a,&r)||liq_set_dithering_level(r,0))return 2;
 unsigned char out[8];if(liq_write_remapped_image(r,i,out,8))return 3;
 const liq_palette*p=liq_get_palette(r);
 for(int k=0;k<8;k++){if(out[k]>=p->count)return 4;liq_color c=p->entries[out[k]];if(c.r!=px[k].r||c.b!=px[k].b||c.a!=255)return 5;}
 std::cout<<"LIQ_EXACT_REMAP_OK pixels=8 colors="<<p->count<<"\n";
 liq_result_destroy(r);liq_image_destroy(i);liq_attr_destroy(a);
}
