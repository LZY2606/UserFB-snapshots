#include "fixture.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"baseline:%d: %s\n",__LINE__,#x);return 1; } } while(0)
int main(int argc,char **argv) {
  gc_bytes bytes;stbtt_fontinfo f;int a,b,w,h,x,y,i;unsigned long sum=0;unsigned char *pixels;
  gc_default_font(&bytes);memset(&f,0,sizeof(f));
  CHECK(stbtt_InitFont(&f,bytes.b,0));CHECK(f.numGlyphs==8);
  a=stbtt_FindGlyphIndex(&f,'f');b=stbtt_FindGlyphIndex(&f,'i');CHECK(a==1 && b==2);
  pixels=stbtt_GetGlyphBitmap(&f,0.04f,0.04f,3,&w,&h,&x,&y);CHECK(pixels && w>0 && h>0);
  for(i=0;i<w*h;i++)sum+=pixels[i];stbtt_FreeBitmap(pixels,f.userdata);CHECK(sum>0);
  CHECK(stbtt__find_table(bytes.b,0,"GSUB")>0);CHECK(stbtt__find_table(bytes.b,0,"GDEF")>0);
  printf("CMAP_RUN=%d,%d COUNT=2\n",a,b);
  printf("DECLARED_LIGA=1,2->3 GLYPH3_BITMAP=%dx%d SUM=%lu\n",w,h,sum);
  if(argc==2) { FILE *fp=fopen(argv[1],"wb");CHECK(fp);CHECK(fwrite(bytes.b,1,(size_t)bytes.n,fp)==(size_t)bytes.n);CHECK(!fclose(fp)); }
  puts("BASELINE_OK");return 0;
}
