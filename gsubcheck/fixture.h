/* Original geometric test material, MIT (same license as the task additions).
   This builds bytes, not a GSUB interpreter. No installed fonts are used. */
#ifndef GSUBCHECK_FIXTURE_H
#define GSUBCHECK_FIXTURE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned char b[32768]; int n; } gc_bytes;
static void gc_u8(gc_bytes *b, unsigned x) { if(b->n >= 32768) abort(); b->b[b->n++]=(unsigned char)x; }
static void gc_u16(gc_bytes *b, unsigned x) { gc_u8(b,x>>8); gc_u8(b,x); }
static void gc_u32(gc_bytes *b, unsigned x) { gc_u16(b,x>>16); gc_u16(b,x); }
static void gc_patch16(gc_bytes *b,int p,unsigned x) { b->b[p]=(unsigned char)(x>>8); b->b[p+1]=(unsigned char)x; }
static void gc_patch32(gc_bytes *b,int p,unsigned x) { gc_patch16(b,p,x>>16); gc_patch16(b,p+2,x); }
static void gc_zero(gc_bytes *b,int n) { while(n--) gc_u8(b,0); }
static unsigned gc_get32(const unsigned char *p) { return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static unsigned gc_sum(const gc_bytes *b) {
  unsigned sum=0; int i;
  for(i=0;i<b->n;i+=4) {
    unsigned v=0; int j;
    for(j=0;j<4;j++) v=(v<<8)|(i+j<b->n ? b->b[i+j]:0);
    sum+=v;
  }
  return sum;
}
static void gc_base_gsub(gc_bytes *b) {
  int script,feature,look;
  b->n=0; gc_u32(b,0x10000); gc_zero(b,6);
  script=b->n; gc_patch16(b,4,(unsigned)script);
  gc_u16(b,1); gc_u32(b,0x44464c54); gc_u16(b,8); /* DFLT */
  gc_u16(b,4); gc_u16(b,0); /* default LangSys, no named languages */
  gc_u16(b,0); gc_u16(b,0xffff); gc_u16(b,1); gc_u16(b,0);
  feature=b->n; gc_patch16(b,6,(unsigned)feature);
  gc_u16(b,1); gc_u32(b,0x6c696761); gc_u16(b,8); /* liga */
  gc_u16(b,0); gc_u16(b,1); gc_u16(b,0);
  look=b->n; gc_patch16(b,8,(unsigned)look);
  gc_u16(b,1); gc_u16(b,4);
  gc_u16(b,4); gc_u16(b,8); gc_u16(b,1); gc_u16(b,8);
  gc_u16(b,1); gc_u16(b,8); gc_u16(b,1); gc_u16(b,14);
  gc_u16(b,1); gc_u16(b,1); gc_u16(b,1); /* coverage: f */
  gc_u16(b,1); gc_u16(b,4); /* ligature set */
  gc_u16(b,3); gc_u16(b,2); gc_u16(b,2); /* f i -> fi */
}
static void gc_base_gdef(gc_bytes *b) {
  static const unsigned cls[8]={0,1,1,2,3,1,1,1}; int i;
  b->n=0; gc_u32(b,0x10000); gc_u16(b,12); gc_zero(b,6);
  gc_u16(b,1); gc_u16(b,0); gc_u16(b,8);
  for(i=0;i<8;i++) gc_u16(b,cls[i]);
}
/* Replace GSUB/GDEF independently to build new public test inputs. */
static void gc_font(gc_bytes *out,const gc_bytes *gsub,const gc_bytes *gdef) {
  static const char tags[11][5]={"GDEF","GSUB","cmap","glyf","head","hhea","hmtx","loca","maxp","name","post"};
  gc_bytes *t=(gc_bytes*)calloc(11,sizeof(gc_bytes));
  int i,j,head_at=0; static const int widths[8]={200,200,100,450,80,250,150,300};
  if(!t) abort();
  t[0]=*gdef; t[1]=*gsub;
  gc_u16(&t[2],0);gc_u16(&t[2],1);gc_u16(&t[2],3);gc_u16(&t[2],1);gc_u32(&t[2],12);
  gc_u16(&t[2],4);gc_u16(&t[2],480);gc_u16(&t[2],0);
  gc_u16(&t[2],4);gc_u16(&t[2],4);gc_u16(&t[2],1);gc_u16(&t[2],0);
  gc_u16(&t[2],255);gc_u16(&t[2],0xffff);gc_u16(&t[2],0);
  gc_u16(&t[2],32);gc_u16(&t[2],0xffff);gc_u16(&t[2],0);gc_u16(&t[2],1);
  gc_u16(&t[2],4);gc_u16(&t[2],0);
  for(i=32;i<256;i++)gc_u16(&t[2],i=='i'?2:1);
  for(i=0;i<8;i++) {
    int w=widths[i]; gc_u32(&t[7],(unsigned)t[3].n);
    gc_u16(&t[3],1); gc_u16(&t[3],0);gc_u16(&t[3],0);gc_u16(&t[3],w);gc_u16(&t[3],600);
    gc_u16(&t[3],3); gc_u16(&t[3],0);
    for(j=0;j<4;j++)gc_u8(&t[3],1);
    gc_u16(&t[3],0);gc_u16(&t[3],w);gc_u16(&t[3],0);gc_u16(&t[3],(unsigned)-w);
    gc_u16(&t[3],0);gc_u16(&t[3],0);gc_u16(&t[3],600);gc_u16(&t[3],0);
    gc_u16(&t[6],w+50);gc_u16(&t[6],0);
  }
  gc_u32(&t[7],(unsigned)t[3].n);
  gc_zero(&t[4],54);gc_patch32(&t[4],0,0x10000);gc_patch32(&t[4],12,0x5f0f3cf5);
  gc_patch16(&t[4],18,1000);gc_patch16(&t[4],40,450);gc_patch16(&t[4],42,600);gc_patch16(&t[4],50,1);
  gc_zero(&t[5],36);gc_patch32(&t[5],0,0x10000);gc_patch16(&t[5],4,800);
  gc_patch16(&t[5],6,(unsigned)-200);gc_patch16(&t[5],34,8);
  gc_zero(&t[8],32);gc_patch32(&t[8],0,0x10000);gc_patch16(&t[8],4,8);
  gc_u16(&t[9],0);gc_u16(&t[9],1);gc_u16(&t[9],18);
  gc_u16(&t[9],3);gc_u16(&t[9],1);gc_u16(&t[9],0x409);gc_u16(&t[9],4);gc_u16(&t[9],8);gc_u16(&t[9],0);
  gc_u16(&t[9],'D');gc_u16(&t[9],'e');gc_u16(&t[9],'m');gc_u16(&t[9],'o');
  gc_u32(&t[10],0x30000);gc_zero(&t[10],28);
  out->n=0;gc_u32(out,0x10000);gc_u16(out,11);gc_u16(out,128);gc_u16(out,3);gc_u16(out,48);
  gc_zero(out,16*11);
  for(i=0;i<11;i++) {
    int r=12+16*i;
    for(j=0;j<4;j++)out->b[r+j]=(unsigned char)tags[i][j];
    gc_patch32(out,r+4,gc_sum(&t[i]));gc_patch32(out,r+8,(unsigned)out->n);gc_patch32(out,r+12,(unsigned)t[i].n);
    if(i==4)head_at=out->n;
    for(j=0;j<t[i].n;j++)gc_u8(out,t[i].b[j]);
    while(out->n%4)gc_u8(out,0);
  }
  gc_patch32(out,head_at+8,0xb1b0afba-gc_sum(out));free(t);
}
static void gc_default_font(gc_bytes *out) {
  gc_bytes gsub,gdef;gc_base_gsub(&gsub);gc_base_gdef(&gdef);gc_font(out,&gsub,&gdef);
}
#endif
