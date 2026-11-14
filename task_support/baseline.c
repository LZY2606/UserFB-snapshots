/* SPDX-License-Identifier: MIT
 * Deterministic normal-feature baseline. Contains no weighted-pred implementation.
 */
#include "kvazaar.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 64
#define H 64
#define N 5
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"baseline:%d: %s\n",__LINE__,#x); return 1; } } while (0)

static unsigned char sample(int frame, int c, int x, int y)
{
  int shift = frame ? frame - 1 : 0;
  return (unsigned char)((13 * ((x + shift) & 63) + 7 * y +
    29 * ((x / 8) ^ (y / 8)) + c * 47 + frame * 9) & 255);
}
static void fill(kvz_picture *p, int frame)
{
  for (int c=0;c<3;++c) {
    int w=c?W/2:W, h=c?H/2:H, stride=c?p->stride_c:p->stride;
    for (int y=0;y<h;++y) for (int x=0;x<w;++x)
      p->data[c][y*stride+x]=sample(frame,c,x,y);
  }
  p->pts=1000+40*frame;
}
static int same(const kvz_picture *p, int frame)
{
  if (!p || p->width!=W || p->height!=H || p->pts!=1000+40*frame) return 0;
  for (int c=0;c<3;++c) {
    int w=c?W/2:W, h=c?H/2:H, stride=c?p->stride_c:p->stride;
    for (int y=0;y<h;++y) for (int x=0;x<w;++x)
      if (p->data[c][y*stride+x]!=sample(frame,c,x,y)) return 0;
  }
  return 1;
}
static uint64_t fnv(uint64_t h, const unsigned char *p, size_t n)
{
  while(n--) { h^=*p++; h*=UINT64_C(1099511628211); } return h;
}
static int run(const kvz_api *api, int owf)
{
  kvz_config *cfg=api->config_alloc(); REQUIRE(cfg && api->config_init(cfg));
  const char *opts[][2]={{"preset","ultrafast"},{"gop","0"},{"ref","1"},
    {"bipred","0"},{"threads","2"},{"wpp","0"},{"cpuid","0"},
    {"lossless","1"},{"deblock","0"},{"sao","off"},{"info","0"}};
  for(size_t i=0;i<sizeof(opts)/sizeof(opts[0]);++i)
    REQUIRE(api->config_parse(cfg,opts[i][0],opts[i][1]));
  cfg->width=W; cfg->height=H; cfg->owf=owf;
  cfg->enable_logging_output=0;
  kvz_encoder *enc=api->encoder_open(cfg); REQUIRE(enc);
  api->config_destroy(cfg);
  int outputs=0, pframes=0, delayed=0, flushed=0;
  size_t total=0; uint64_t hash=UINT64_C(14695981039346656037);
  char filename[80]; snprintf(filename,sizeof(filename),".task-build/baseline-owf%d.hevc",owf);
  FILE *out=fopen(filename,"wb"); REQUIRE(out);
  char golden[80]; snprintf(golden,sizeof(golden),"task_support/legacy-owf%d.hevc",owf);
  FILE *expected=fopen(golden,"rb"); REQUIRE(expected);
  for(int i=0;i<N+N+4;++i) {
    kvz_picture *in=NULL,*rec=NULL,*src=NULL; kvz_data_chunk *chunks=NULL;
    uint32_t bytes=0; kvz_frame_info info;
    if(i<N) { in=api->picture_alloc(W,H); REQUIRE(in); fill(in,i); }
    REQUIRE(api->encoder_encode(enc,in,&chunks,&bytes,&rec,&src,&info));
    if(in) api->picture_free(in);
    if(!chunks) {
      REQUIRE(bytes==0 && !rec && !src);
      if(i<N) { ++delayed; continue; }
      REQUIRE(outputs==N); break;
    }
    REQUIRE(outputs<N && bytes>0 && same(rec,outputs) && same(src,outputs));
    REQUIRE(info.poc==outputs);
    if(outputs==0) REQUIRE(info.slice_type==KVZ_SLICE_I);
    else {
      REQUIRE(info.slice_type==KVZ_SLICE_P);
      REQUIRE(info.ref_list_len[0]==1 && info.ref_list[0][0]==outputs-1);
      ++pframes;
    }
    size_t actual=0;
    for(kvz_data_chunk *c=chunks;c;c=c->next) {
      REQUIRE(c->len>0);
      REQUIRE(fwrite(c->data,1,c->len,out)==c->len);
      for (uint32_t j=0;j<c->len;++j) REQUIRE(fgetc(expected)==c->data[j]);
      hash=fnv(hash,c->data,c->len); actual+=c->len;
    }
    REQUIRE(actual==bytes); total+=bytes;
    api->chunk_free(chunks); api->picture_free(rec); api->picture_free(src);
    ++outputs; if(i>=N) ++flushed;
  }
  REQUIRE(fgetc(expected)==EOF && !ferror(expected));
  REQUIRE(fclose(expected)==0);
  REQUIRE(fclose(out)==0 && outputs==N && pframes==N-1 && total>100);
  for(int i=0;i<2;++i) {
    kvz_picture *rec=NULL,*src=NULL; kvz_data_chunk *chunks=NULL; uint32_t bytes=99;
    REQUIRE(api->encoder_encode(enc,NULL,&chunks,&bytes,&rec,&src,NULL));
    REQUIRE(!chunks && !bytes && !rec && !src);
  }
  if(owf) REQUIRE(delayed>0 && flushed>0);
  api->encoder_close(enc);
  printf("BASELINE owf=%d frames=%d P=%d reconstructed_bytes=%d bytes=%zu fnv=%016llx delayed=%d flushed=%d COMPLETE\n",
    owf,outputs,pframes,N*W*H*3/2,total,(unsigned long long)hash,delayed,flushed);
  return 0;
}
int main(void)
{
  const kvz_api *api=kvz_api_get(8); REQUIRE(api);
  if(run(api,0) || run(api,1)) return 1;
  puts("BASELINE_COMPLETE"); return 0;
}
