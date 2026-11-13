/* starting-point witness, CC0. No target stereo implementation. */
#include <xm.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void u16(unsigned char *p, unsigned v) {p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void u32(unsigned char *p, unsigned v) {u16(p,v);u16(p+2,v>>16);}
static void fixture(unsigned char *p, bool stereo) {
  memset(p,0,1024); memcpy(p,"check generated PCM",19);
  p[28]=0x1a; p[29]=0x10; u16(p+32,2); u16(p+34,1); u16(p+36,1);
  u16(p+40,0x5000);u16(p+42,2);memcpy(p+44,"SCRM",4);
  p[48]=64;p[49]=6;p[50]=125;p[51]=0xc0;
  memset(p+64,255,32);p[64]=0;p[96]=0;p[97]=255;
  u16(p+98,0x10);u16(p+100,0x20);
  unsigned char *i=p+256;i[0]=1;u16(i+14,0x30);u32(i+16,16);
  u32(i+20,0);u32(i+24,16);i[28]=64;i[31]=(unsigned char)(1+(stereo?2:0));
  u32(i+32,8363);memcpy(i+76,"SCRS",4);
  u16(p+512,71);p[514]=0xe0;p[515]=0x40;p[516]=1;p[517]=64;
  p[518]=24;p[519]=64; /* X40: center pan */
  for(unsigned k=0;k<16;k++){p[768+k]=(unsigned char)(32+12*k);p[784+k]=(unsigned char)(224-12*k);}
}
static xm_context_t *load(const unsigned char *d) {
  xm_prescan_data_t *p=malloc(XM_PRESCAN_DATA_SIZE);assert(p);
  assert(xm_prescan_module((const char*)d,1024,p));
  void *mem=malloc(xm_size_for_context(p));assert(mem);
  xm_context_t *ctx=xm_create_context(mem,p,(const char*)d,1024);free(p);
  xm_set_sample_rate(ctx,48000);return ctx;
}
int main(void) {
  unsigned char mono[1024],stereo[1024];fixture(mono,false);fixture(stereo,true);
  xm_context_t *a=load(mono),*b=load(stereo);
  float x[8192],y[8192];xm_generate_samples(a,x,4096);xm_generate_samples(b,y,4096);
  double energy=0;for(unsigned k=0;k<8192;k++){assert(isfinite(x[k]));energy+=fabs((double)x[k]);}
  assert(energy>1);assert(memcmp(x,y,sizeof x)==0);
  uint32_t n=0;xm_sample_point_t *w=xm_get_sample_waveform(b,0,&n);
  assert(n==16);assert(w[0]==-24576 && w[15]==21504);
  printf("S3M_START frames=4096 waveform=16 nonzero=1 stereo_equals_left_only=1\n");
  xm_reset_context(a);xm_set_sample_rate(a,48000);
  uint32_t sz=xm_save_size(a);char *saved=malloc(sz);assert(saved);xm_save_context(a,saved);
  xm_prescan_data_t *scan=malloc(XM_PRESCAN_DATA_SIZE);assert(scan);
  assert(xm_prescan_module(saved,sz,scan));void *mem=malloc(xm_size_for_context(scan));assert(mem);
  xm_context_t *c=xm_create_context(mem,scan,saved,sz);xm_set_sample_rate(c,48000);
  xm_generate_samples(a,x,4096);xm_generate_samples(c,y,4096);assert(memcmp(x,y,sizeof x)==0);
  printf("S3M_MONO_SAVE roundtrip_pcm_equal=1\n");
  free(a);free(b);free(c);free(saved);free(scan);return 0;
}
