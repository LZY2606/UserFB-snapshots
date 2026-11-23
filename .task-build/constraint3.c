#include "kvazaar.h"
#include <stdio.h>
int main(void){
  const kvz_api *api = kvz_api_get(8);
  kvz_config *c = api->config_alloc(); api->config_init(c);
  const char *o[][2] = {{"gop","0"},{"ref","1"},{"bipred","0"},{"wpp","0"},{"weighted-pred","1"},{"hash","none"}};
  for (size_t i=0;i<sizeof(o)/sizeof(o[0]);++i) api->config_parse(c,o[i][0],o[i][1]);
  c->width=32;c->height=32;c->owf=0;c->threads=1;c->enable_logging_output=0;
  kvz_encoder *e = api->encoder_open(c); api->config_destroy(c);
  kvz_picture *p = api->picture_alloc(32,32);
  p->wp.enabled=1; p->wp.denom=8;
  kvz_data_chunk *ch=NULL; uint32_t b=99; kvz_picture *r=NULL,*s=NULL;
  int rc = api->encoder_encode(e,p,&ch,&b,&r,&s,NULL);
  printf("bad-call rc=%d (expect 0)\n",rc);
  p->wp.enabled=0; ch=NULL; b=99; r=NULL; s=NULL;
  rc = api->encoder_encode(e,p,&ch,&b,&r,&s,NULL);
  printf("retry rc=%d ch=%p b=%u\n",rc,(void*)ch,b);
  if(ch){api->chunk_free(ch);api->picture_free(r);api->picture_free(s);}
  api->picture_free(p); api->encoder_close(e);
  return 0;
}
