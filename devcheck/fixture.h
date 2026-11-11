/* Original geometric sfnt fixture, MIT licensed. No system font data. */
#ifndef COMPOUND_FIXTURE_H
#define COMPOUND_FIXTURE_H
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned char bytes[4096]; int size; } cf_blob;
static void cf_u16(unsigned char *p, int v) { p[0]=(unsigned char)(v>>8); p[1]=(unsigned char)v; }
static void cf_u32(unsigned char *p, uint32_t v) { p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v; }
static uint32_t cf_sum(const unsigned char *p, int n) {
 uint32_t s=0; int i,j; for(i=0;i<n;i+=4) { uint32_t v=0; for(j=0;j<4;j++) v=(v<<8)|(i+j<n?p[i+j]:0); s+=v; } return s;
}
static void cf_header(cf_blob *g,int contours,int x0,int y0,int x1,int y1) {
 memset(g,0,sizeof(*g)); cf_u16(g->bytes,contours); cf_u16(g->bytes+2,x0); cf_u16(g->bytes+4,y0); cf_u16(g->bytes+6,x1); cf_u16(g->bytes+8,y1); g->size=10;
}
/* Signed 16-bit deltas deliberately avoid compressed-coordinate helpers. */
static void cf_simple(cf_blob *g,int n,const int *xy,const unsigned char *on,int x0,int y0,int x1,int y1) {
 int i,k,prev; cf_header(g,1,x0,y0,x1,y1); cf_u16(g->bytes+10,n-1); cf_u16(g->bytes+12,0); g->size=14;
 for(i=0;i<n;i++) g->bytes[g->size++]=on[i];
 for(k=0;k<2;k++) { prev=0; for(i=0;i<n;i++) { int v=xy[2*i+k]; cf_u16(g->bytes+g->size,v-prev); g->size+=2; prev=v; } }
}
static void cf_component(cf_blob *g,int flags,int glyph,int a,int b,int scale) {
 cf_u16(g->bytes+g->size,flags); cf_u16(g->bytes+g->size+2,glyph); g->size+=4;
 if(flags&1) { cf_u16(g->bytes+g->size,a); cf_u16(g->bytes+g->size+2,b); g->size+=4; }
 else { g->bytes[g->size++]=(unsigned char)a; g->bytes[g->size++]=(unsigned char)b; }
 if(flags&8) { cf_u16(g->bytes+g->size,scale); g->size+=2; }
}
static void cf_make_font(cf_blob *out, int xy_only) {
 static const char *tags[]={"cmap","glyf","head","hhea","hmtx","loca","maxp"};
 static const int base[]={0,0,80,0,80,80,0,80}, mark[]={0,0,20,0,0,20};
 static const unsigned char base_on[]={0,0,1,1}, mark_on[]={1,1,1};
 cf_blob tab[7],g[6]; int i,j,off,head_off=0;
 memset(tab,0,sizeof(tab)); memset(g,0,sizeof(g)); memset(out,0,sizeof(*out));
 cf_simple(&g[1],4,base,base_on,0,0,80,80); cf_simple(&g[2],3,mark,mark_on,0,0,20,20);
 cf_header(&g[3],-1,0,-20,100,80); cf_component(&g[3],35,1,0,0,0); cf_component(&g[3],3,2,80,-20,0);
 cf_header(&g[4],-1,0,-20,100,80); cf_component(&g[4],35,1,0,0,0); cf_component(&g[4],xy_only?3:1,2,xy_only?80:1,xy_only?-20:2,0);
 cf_header(&g[5],-1,20,0,60,40); cf_component(&g[5],0x100b,1,20,0,8192);
 /* cmap format 4: A..E => 1..5, u => 1 (upstream SDF sample), sentinel. */
 cf_u16(tab[0].bytes+2,1); cf_u16(tab[0].bytes+4,3); cf_u16(tab[0].bytes+6,1); cf_u32(tab[0].bytes+8,12);
 cf_u16(tab[0].bytes+12,4); cf_u16(tab[0].bytes+14,40); cf_u16(tab[0].bytes+18,6);
 cf_u16(tab[0].bytes+20,4); cf_u16(tab[0].bytes+22,1); cf_u16(tab[0].bytes+24,2);
 cf_u16(tab[0].bytes+26,'E'); cf_u16(tab[0].bytes+28,'u'); cf_u16(tab[0].bytes+30,65535);
 cf_u16(tab[0].bytes+34,'A'); cf_u16(tab[0].bytes+36,'u'); cf_u16(tab[0].bytes+38,65535);
 cf_u16(tab[0].bytes+40,1-'A'); cf_u16(tab[0].bytes+42,1-'u'); cf_u16(tab[0].bytes+44,1); tab[0].size=52;
 for(i=0;i<6;i++) { cf_u32(tab[5].bytes+4*i,(uint32_t)tab[1].size); memcpy(tab[1].bytes+tab[1].size,g[i].bytes,(size_t)g[i].size); tab[1].size=(tab[1].size+g[i].size+3)&~3; }
 cf_u32(tab[5].bytes+24,(uint32_t)tab[1].size); tab[5].size=28;
 cf_u32(tab[2].bytes,0x00010000); cf_u32(tab[2].bytes+4,0x00010000); cf_u32(tab[2].bytes+12,0x5f0f3cf5); cf_u16(tab[2].bytes+18,1000);
 cf_u16(tab[2].bytes+38,-20); cf_u16(tab[2].bytes+40,100); cf_u16(tab[2].bytes+42,80); cf_u16(tab[2].bytes+46,8); cf_u16(tab[2].bytes+48,2); cf_u16(tab[2].bytes+50,1); tab[2].size=54;
 cf_u32(tab[3].bytes,0x00010000); cf_u16(tab[3].bytes+4,800); cf_u16(tab[3].bytes+6,-200); cf_u16(tab[3].bytes+10,120); cf_u16(tab[3].bytes+18,1); cf_u16(tab[3].bytes+34,6); tab[3].size=36;
 for(i=0;i<6;i++) { cf_u16(tab[4].bytes+4*i,120); cf_u16(tab[4].bytes+4*i+2,i==5?20:0); } tab[4].size=24;
 cf_u32(tab[6].bytes,0x00010000); cf_u16(tab[6].bytes+4,6); cf_u16(tab[6].bytes+6,4); cf_u16(tab[6].bytes+8,1); cf_u16(tab[6].bytes+10,7); cf_u16(tab[6].bytes+12,2); cf_u16(tab[6].bytes+14,2); cf_u16(tab[6].bytes+28,2); cf_u16(tab[6].bytes+30,1); tab[6].size=32;
 cf_u32(out->bytes,0x00010000); cf_u16(out->bytes+4,7); cf_u16(out->bytes+6,64); cf_u16(out->bytes+8,2); cf_u16(out->bytes+10,48); off=12+7*16;
 for(i=0;i<7;i++) { unsigned char *r=out->bytes+12+i*16; memcpy(r,tags[i],4); cf_u32(r+4,cf_sum(tab[i].bytes,tab[i].size)); cf_u32(r+8,(uint32_t)off); cf_u32(r+12,(uint32_t)tab[i].size); memcpy(out->bytes+off,tab[i].bytes,(size_t)tab[i].size); if(i==2) head_off=off; off=(off+tab[i].size+3)&~3; }
 out->size=off; cf_u32(out->bytes+head_off+8,0xb1b0afba-cf_sum(out->bytes,out->size));
 assert(cf_sum(out->bytes,out->size)==0xb1b0afba);
 /* Directory checksums: head is calculated with checkSumAdjustment zero. */
 for(i=0;i<7;i++) { const unsigned char *r=out->bytes+12+i*16; uint32_t stored=0; for(j=0;j<4;j++) stored=(stored<<8)|r[4+j]; assert(stored==cf_sum(tab[i].bytes,tab[i].size)); }
}
#endif
