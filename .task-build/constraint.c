#include "kvazaar.h"
#include <stdio.h>
static const kvz_api *api;
static kvz_encoder *try_open(const char *k, const char *v) {
  kvz_config *c = api->config_alloc(); api->config_init(c);
  const char *o[][2] = {{"gop","0"},{"ref","1"},{"bipred","0"},{"wpp","0"},{"weighted-pred","1"}};
  for (size_t i=0;i<sizeof(o)/sizeof(o[0]);++i) api->config_parse(c,o[i][0],o[i][1]);
  api->config_parse(c,k,v);
  c->width=32;c->height=32;c->enable_logging_output=0;
  kvz_encoder *e = api->encoder_open(c);
  api->config_destroy(c);
  return e;
}
int main(void){
  api = kvz_api_get(8);
  if (try_open("gop","8")) { printf("FAIL: gop=8 accepted\n"); return 1; }
  if (try_open("ref","2")) { printf("FAIL: ref=2 accepted\n"); return 1; }
  if (try_open("bipred","1")) { printf("FAIL: bipred=1 accepted\n"); return 1; }
  kvz_encoder *e = try_open("x","x"); // dummy, valid combo
  if (!e) { printf("FAIL: valid combo rejected\n"); return 1; }
  api->encoder_close(e);
  // invalid wp params: encode must fail before consuming
  kvz_config *c = api->config_alloc(); api->config_init(c);
  const char *o[][2] = {{"gop","0"},{"ref","1"},{"bipred","0"},{"wpp","0"},{"weighted-pred","1"},{"hash","none"}};
  for (size_t i=0;i<sizeof(o)/sizeof(o[0]);++i) api->config_parse(c,o[i][0],o[i][1]);
  c->width=32;c->height=32;c->owf=0;c->threads=1;c->enable_logging_output=0;
  e = api->encoder_open(c); api->config_destroy(c);
  kvz_picture *p = api->picture_alloc(32,32);
  p->wp.enabled=1; p->wp.denom=8; // bad denom
  kvz_data_chunk *ch=NULL; uint32_t b=99; kvz_picture *r=NULL,*s=NULL;
  int rc = api->encoder_encode(e,p,&ch,&b,&r,&s,NULL);
  if (rc!=0 || ch || b!=0 || r || s) { printf("FAIL: invalid wp accepted rc=%d\n",rc); return 1; }
  // picture not consumed: can still encode it with corrected params
  p->wp.enabled=0;
  rc = api->encoder_encode(e,p,&ch,&b,&r,&s,NULL);
  printf("retry rc=%d ch=%p b=%u\n",rc,(void*)ch,b);
  if (rc!=1 || !ch) { printf("FAIL: retry after bad wp failed rc=%d\n",rc); return 1; }
  api->chunk_free(ch); api->picture_free(r); api->picture_free(s);
  api->picture_free(p); api->encoder_close(e);
  printf("CONSTRAINT_OK\n");
  return 0;
}
