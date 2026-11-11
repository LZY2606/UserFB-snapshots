/* MIT; starting-state witness only. No target implementation. */
#include "fixture.h"
#include <stdlib.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
int main(int argc,char **argv) {
 cf_blob font; stbtt_fontinfo info; stbtt_vertex *v=NULL; unsigned char *p; int n,w,h,x,y,i,nonzero=0;
 cf_make_font(&font,0); memset(&info,0,sizeof(info)); assert(stbtt_InitFont(&info,font.bytes,0));
 if(argc>1 && (!strcmp(argv[1],"--font") || !strcmp(argv[1],"--normal-font"))) { FILE *f; if(!strcmp(argv[1],"--normal-font")) cf_make_font(&font,1); f=fopen("build/geometric.ttf","wb"); assert(f); assert(fwrite(font.bytes,1,(size_t)font.size,f)==(size_t)font.size); fclose(f); return 0; }
 if(argc>1 && !strcmp(argv[1],"--anchor")) { n=stbtt_GetGlyphShape(&info,4,&v); printf("ANCHOR_RETURNED %d\n",n); stbtt_FreeShape(&info,v); return 0; }
 assert(stbtt_FindGlyphIndex(&info,'A')==1 && stbtt_FindGlyphIndex(&info,'D')==4);
 n=stbtt_GetGlyphShape(&info,1,&v); assert(n==4 && v[0].type==STBTT_vmove && v[0].x==40 && v[0].y==0); stbtt_FreeShape(&info,v);
 p=stbtt_GetGlyphBitmap(&info,0.25f,0.25f,3,&w,&h,&x,&y); assert(p && w==25 && h==25);
 for(i=0;i<w*h;i++) if(p[i]) nonzero++; assert(nonzero>0); stbtt_FreeBitmap(p,info.userdata);
 printf("NORMAL_OK simple_commands=%d xy_bitmap=%dx%d nonzero=%d\n",n,w,h,nonzero);
 n=stbtt_GetGlyphShape(&info,5,&v); assert(n==4); printf("SCALE_OBSERVED first=(%d,%d) spec=(40,0)\n",v[0].x,v[0].y); assert(v[0].x==20 && v[0].y==0); stbtt_FreeShape(&info,v);
 printf("SFNT_CHECKSUM_OK bytes=%d\n",font.size); return 0;
}
