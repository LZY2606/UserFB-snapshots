#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define NANOSVG_IMPLEMENTATION
#include "../src/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../src/nanosvgrast.h"
int main(void){
 char input[]="<svg width='24' height='16'><defs><linearGradient id='g' x1='0' y1='0' x2='100%' y2='0%'><stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs><rect x='0' y='0' width='24' height='8' fill='url(#g)'/><rect x='0' y='8' width='8' height='8' fill='#00ff00'/><rect x='8' y='8' width='8' height='8' fill='none' stroke='red' stroke-width='2'/></svg>";
 NSVGimage *im=nsvgParse(input,"px",96);NSVGrasterizer *r=nsvgCreateRasterizer();assert(im && r);
 unsigned char image[16*104];memset(image,77,sizeof(image));
 nsvgRasterize(r,im,0,0,1,image,24,16,104);
 assert(image[4*104+2*4]>image[4*104+2*4+2]);
 assert(image[4*104+21*4+2]>image[4*104+21*4]);
 assert(image[12*104+4*4]==0 && image[12*104+4*4+1]==255 && image[12*104+4*4+3]==255);
 assert(image[12*104+12*4+3]==0);
 for(int y=0;y<16;y++)for(int x=96;x<104;x++)assert(image[y*104+x]==77);
 assert(im->shapes && im->shapes->next && im->shapes->next->next && !im->shapes->next->next->next);
 nsvgDeleteRasterizer(r);nsvgDelete(im);puts("NSVG_LEGACY_GRADIENT_STROKE_OK");return 0;
}
