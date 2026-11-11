#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define NANOSVG_IMPLEMENTATION
#include "../src/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../src/nanosvgrast.h"
static void draw(NSVGrasterizer *r,const char *xml,unsigned char *pixels){
 char *s=malloc(strlen(xml)+1);assert(s);strcpy(s,xml);
 NSVGimage *im=nsvgParse(s,"px",96);assert(im);nsvgRasterize(r,im,0,0,1,pixels,24,16,96);
 nsvgDelete(im);free(s);
}
int main(void){
 unsigned char out[24*16*4];NSVGrasterizer *r=nsvgCreateRasterizer();assert(r);
 draw(r,"<svg width='24' height='16'><g opacity='.5'><rect x='1' y='1' width='14' height='12' fill='red'/><rect x='8' y='1' width='14' height='12' fill='blue'/></g></svg>",out);
 unsigned a=out[(6*24+4)*4+3],b=out[(6*24+10)*4+3];
 assert(a>=126 && a<=128);assert(b>a+50);
 printf("NSVG_BASELINE_GROUP_BUG left-alpha=%u overlap-alpha=%u\n",a,b);
 draw(r,"<svg width='24' height='16'><g opacity='.5'><g opacity='.5'><rect x='1' y='1' width='14' height='12' fill='red'/></g></g></svg>",out);
 unsigned c=out[(6*24+4)*4+3];assert(c>=126 && c<=128);
 printf("NSVG_BASELINE_NESTED_BUG alpha=%u expected-near=64\n",c);
 nsvgDeleteRasterizer(r);return 0;
}
