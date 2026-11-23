#include "kvazaar.h"
#include <stdio.h>
int main(void){
  const kvz_api*api=kvz_api_get(8);
  kvz_config*c=api->config_alloc();api->config_init(c);
  const char*o[][2]={{"gop","0"},{"ref","1"},{"bipred","0"},{"weighted-pred","1"},{"input-format","P400"}};
  for(size_t i=0;i<sizeof(o)/sizeof(o[0]);++i)if(!api->config_parse(c,o[i][0],o[i][1]))printf("parse fail %s\n",o[i][0]);
  c->width=32;c->height=32;c->enable_logging_output=0;
  kvz_encoder*e=api->encoder_open(c);
  printf("P400+wp -> %s\n", e?"ACCEPTED(BAD)":"rejected(ok)");
  if(e)api->encoder_close(e);
  api->config_destroy(c);
  return 0;}
