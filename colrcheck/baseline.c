/* SPDX-License-Identifier: MIT. Original input witness, not a color renderer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"baseline:%d: %s\n",__LINE__,#x); return 1; } } while(0)
static unsigned u16(const unsigned char *p){return p[0]*256u+p[1];}
static unsigned u32(const unsigned char *p){return (unsigned)p[0]<<24|(unsigned)p[1]<<16|(unsigned)p[2]<<8|p[3];}
static unsigned table(const unsigned char *b,const char *tag){unsigned i;for(i=0;i<u16(b+4);i++){const unsigned char *r=b+12+16*i;if(!memcmp(r,tag,4))return u32(r+8);}return 0;}
int main(int argc,char **argv){
 unsigned char bytes[4096],*bitmap,*sdf;FILE *f;size_t size;stbtt_fontinfo info;stbtt_vertex *v=NULL;int n,w,h,x,y,i,painted=0;unsigned co,cp;
 CHECK(argc==2);f=fopen(argv[1],"rb");CHECK(f);size=fread(bytes,1,sizeof(bytes),f);CHECK(!ferror(f));CHECK(feof(f));fclose(f);CHECK(size==968);
 co=table(bytes,"COLR");cp=table(bytes,"CPAL");CHECK(co&&cp);CHECK(u16(bytes+co)==1);CHECK(u16(bytes+cp)==0);CHECK(u16(bytes+cp+4)==2);
 CHECK(u32(bytes+co+u32(bytes+co+14))==4);CHECK(bytes[co+u32(bytes+co+14)+u32(bytes+co+u32(bytes+co+14)+6)]==1);
 CHECK(stbtt_InitFont(&info,bytes,0));CHECK(info.numGlyphs==8);CHECK(stbtt_FindGlyphIndex(&info,'A')==1);CHECK(stbtt_FindGlyphIndex(&info,'@')==2);
 n=stbtt_GetGlyphShape(&info,2,&v);CHECK(n==5&&v);stbtt_FreeShape(&info,v);
 bitmap=stbtt_GetGlyphBitmap(&info,.05f,.05f,2,&w,&h,&x,&y);CHECK(bitmap&&w==40&&h==40&&x==0&&y==-40);for(i=0;i<w*h;i++)painted+=bitmap[i]==255;CHECK(painted==1600);stbtt_FreeBitmap(bitmap,NULL);
 printf("MONO_RECT_OK pixels=%d box=0,-40,40,0\n",painted);
 sdf=stbtt_GetGlyphSDF(&info,.05f,2,4,128,32,&w,&h,&x,&y);CHECK(sdf&&w==48&&h==48);CHECK(sdf[24*w+24]>128&&sdf[0]<128);stbtt_FreeSDF(sdf,NULL);puts("MONO_SDF_OK width=48 height=48");
 bitmap=stbtt_GetGlyphBitmap(&info,.05f,.05f,1,&w,&h,&x,&y);CHECK(bitmap==NULL&&w==0&&h==0);
 puts("COLR_INPUT_PRESENT version=1 palettes=2 roots=4 mono_base_pixels=0");printf("BASELINE_OK checks=%d\n",checks);return 0;
}
